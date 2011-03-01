/*
 * libdmtx - Data Matrix Encoding/Decoding Library
 *
 * Copyright (C) 2011 Mike Laughton
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact: mike@dragonflylogic.com
 */

/* $Id$ */

/**
 * this file deals with encoding logic (scheme rules)
 *
 * In the context of this file:
 *
 * A "word" refers to a full codeword byte to be appended to the encoded output.
 *
 * A "value" refers to any scheme value being appended to the output stream,
 * regardless of how many bytes are used to represent it. Examples:
 *
 *   ASCII:                   1 value  in  1 word
 *   ASCII (digits):          2 values in  1 word
 *   C40/Text/X12:            3 values in  2 words
 *   C40/Text/X12 (unlatch):  1 values in  1 word
 *   EDIFACT:                 4 values in  3 words
 *   Base 256:                1 value  in  1 word
 *
 *   - Shifts count as values, so outputChainValueCount will reflect these.
 *
 *   - Latches and unlatches are also counted as values, but always in the
 *     scheme being exited.
 *
 *   - Base256 header bytes are not included as values.
 *
 * A "chunk" refers to the minimum grouping of values in a schema that must be
 * encoded together.
 *
 *   ASCII:                   1 value  (1 word)  in 1 chunk
 *   ASCII (digits):          2 values (1 word)  in 1 chunk (optional)
 *   C40/Text/X12:            3 values (2 words) in 1 chunk
 *   C40/Text/X12 (unlatch):  1 value  (1 word)  in 1 chunk
 *   EDIFACT:                 1 value  (1 word*) in 1 chunk
 *   Base 256:                1 value  (1 word)  in 1 chunk
 *
 *   * EDIFACT writes 6 bits at a time, but progress is tracked to the next byte
 *     boundary. If unlatch value finishes mid-byte, the remaining bits before
 *     the next boundary are all set to zero.
 *
 * XXX maybe reorder the functions list in the file and break them up:
 *
 * Each scheme implements 3 equivalent functions:
 *   - EncodeNextChunk[Scheme]
 *   - EncodeValue[Scheme]
 *   - CompleteIfDone[Scheme]
 *
 * XXX what about renaming EncodeValue[Scheme] to AppendValue[Scheme]? That
 * shows that the stream is being affected
 *
 * The master function EncodeNextChunk() (no Scheme in the name) knows which
 * scheme-specific implementations to call based on the stream's current
 * encodation scheme.
 *
 * It's important that EncodeNextChunk[Scheme] not call CompleteIfDone[Scheme]
 * directly because some parts of the logic might want to encode a stream
 * without allowing the padding and other extra logic that can occur when an
 * end-of-symbol condition is triggered.
 */

#include "dmtx.h"
#include "dmtxstatic.h"

/* XXX is there a way to handle muliple values of s? */
#define CHKSCHEME(s) { if(stream->currentScheme != (s)) { StreamMarkFatal(stream, 1); return; } }

/* CHKERR should follow any call that might alter stream status */
#define CHKERR { if(stream->status != DmtxStatusEncoding) { return; } }

/* CHKSIZE should follows typical calls to FindSymbolSize()  */
#define CHKSIZE { if(sizeIdx == DmtxUndefined) { StreamMarkInvalid(stream, 1); return; } }

/**
 *
 *
 */
static DmtxPassFail
EncodeSingleScheme2(DmtxEncodeStream *stream, DmtxScheme targetScheme, int requestedSizeIdx)
{
   if(stream->currentScheme != DmtxSchemeAscii)
   {
      StreamMarkFatal(stream, 1);
      return DmtxFail;
   }

   while(stream->status == DmtxStatusEncoding)
      EncodeNextChunk(stream, targetScheme, requestedSizeIdx);

   if(stream->status != DmtxStatusComplete || StreamInputHasNext(stream))
      return DmtxFail; /* throw out an error too? */

   return DmtxPass;
}

/**
 * This function distributes work to the equivalent scheme-specific
 * implementation.
 *
 * Each of these functions will encode the next symbol input word, and in some
 * cases this requires additional input words to be encoded as well.
 */
static void
EncodeNextChunk(DmtxEncodeStream *stream, DmtxScheme targetScheme, int requestedSizeIdx)
{
   /* Change to target scheme if necessary */
   if(stream->currentScheme != targetScheme)
   {
      EncodeChangeScheme(stream, targetScheme, DmtxUnlatchExplicit); CHKERR;
      CHKSCHEME(targetScheme);
   }

   switch(stream->currentScheme)
   {
      case DmtxSchemeAscii:
         EncodeNextChunkAscii(stream); CHKERR;
         CompleteIfDoneAscii(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeC40:
      case DmtxSchemeText:
      case DmtxSchemeX12:
         EncodeNextChunkCTX(stream, requestedSizeIdx); CHKERR;
         CompleteIfDoneCTX(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeEdifact:
         EncodeNextChunkEdifact(stream); CHKERR;
         CompleteIfDoneEdifact(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeBase256:
         EncodeNextChunkBase256(stream); CHKERR;
         CompleteIfDoneBase256(stream, requestedSizeIdx); CHKERR;
         break;
      default:
         StreamMarkFatal(stream, 1 /* unknown */);
         break;
   }
}

/**
 *
 *
 */
static void
EncodeChangeScheme(DmtxEncodeStream *stream, DmtxScheme targetScheme, int unlatchType)
{
   /* Nothing to do */
   if(stream->currentScheme == targetScheme)
      return;

   /* Every latch must go through ASCII */
   switch(stream->currentScheme)
   {
      case DmtxSchemeC40:
      case DmtxSchemeText:
      case DmtxSchemeX12:
         if(unlatchType == DmtxUnlatchExplicit)
         {
            EncodeUnlatchCTX(stream); CHKERR;
         }
         break;
      case DmtxSchemeEdifact:
         if(unlatchType == DmtxUnlatchExplicit)
         {
            EncodeValueEdifact(stream, DmtxValueEdifactUnlatch); CHKERR;
         }
         break;
      default:
         /* Nothing to do for ASCII or Base 256 */
         assert(stream->currentScheme == DmtxSchemeAscii ||
               stream->currentScheme == DmtxSchemeBase256);
         break;
   }
   stream->currentScheme = DmtxSchemeAscii;

   /* Anything other than ASCII (the default) requires a latch */
   switch(targetScheme)
   {
      case DmtxSchemeC40:
         EncodeValueAscii(stream, DmtxValueC40Latch); CHKERR;
         break;
      case DmtxSchemeText:
         EncodeValueAscii(stream, DmtxValueTextLatch); CHKERR;
         break;
      case DmtxSchemeX12:
         EncodeValueAscii(stream, DmtxValueX12Latch); CHKERR;
         break;
      case DmtxSchemeEdifact:
         EncodeValueAscii(stream, DmtxValueEdifactLatch); CHKERR;
         break;
      case DmtxSchemeBase256:
         EncodeValueAscii(stream, DmtxValueBase256Latch); CHKERR;
         break;
      default:
         /* Nothing to do for ASCII */
         CHKSCHEME(DmtxSchemeAscii);
         break;
   }
   stream->currentScheme = targetScheme;

   /* Reset new chain length to zero */
   stream->outputChainWordCount = 0;
   stream->outputChainValueCount = 0;

   /* Insert header byte if just latched to Base256 */
   if(targetScheme == DmtxSchemeBase256)
   {
      UpdateBase256ChainHeader(stream, DmtxUndefined); CHKERR;
   }
}

/**
 * \brief  Randomize 253 state
 * \param  codewordValue
 * \param  codewordPosition
 * \return Randomized value
 */
static DmtxByte
Randomize253State2(DmtxByte cwValue, int cwPosition)
{
   int pseudoRandom, tmp;

   pseudoRandom = ((149 * cwPosition) % 253) + 1;
   tmp = cwValue + pseudoRandom;
   if(tmp > 254)
      tmp -= 254;

   assert(tmp >= 0 && tmp < 256);

   return (DmtxByte)tmp;
}

/**
 * \brief  Randomize 255 state
 * \param  value
 * \param  position
 * \return Randomized value
 */
static DmtxByte
Randomize255State2(DmtxByte value, int position)
{
   int pseudoRandom, tmp;

   pseudoRandom = ((149 * position) % 255) + 1;
   tmp = value + pseudoRandom;

   return (tmp <= 255) ? tmp : tmp - 256;
}

/**
 *
 *
 */
static int
GetRemainingSymbolCapacity(int outputLength, int sizeIdx)
{
   int capacity;
   int remaining;

   if(sizeIdx == DmtxUndefined)
   {
      remaining = DmtxUndefined;
   }
   else
   {
      capacity = dmtxGetSymbolAttribute(DmtxSymAttribSymbolDataWords, sizeIdx);
      remaining = capacity - outputLength;
   }

   return remaining;
}