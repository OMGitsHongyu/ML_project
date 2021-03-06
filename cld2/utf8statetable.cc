// Copyright 2013 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// State Table follower for scanning UTF-8 strings without converting to
// 32- or 16-bit Unicode values.
//

#include "utf8statetable.h"
#include "stringpiece.h"
#include "unaligned_access.h"

#include <stdint.h>
#include <string.h>

namespace CLD2 {

static const int kReplaceAndResumeFlag = 0x80; // Bit in del byte to distinguish
                                               // optional next-state field
                                               // after replacement text
static const int kHtmlPlaintextFlag = 0x80;    // Bit in add byte to distinguish
                                               // HTML replacement vs. plaintext

extern const unsigned char kUTF8LenTbl[256] = {
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,

  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4, 4,4,4,4,4,4,4,4
};

/**
 * This code implements a little interpreter for UTF8 state
 * tables. There are three kinds of quite-similar state tables,
 * property, scanning, and replacement. Each state in one of
 * these tables consists of an array of 256 or 64 one-byte
 * entries. The state is subscripted by an incoming source byte,
 * and the entry either specifies the next state or specifies an
 * action. Space-optimized tables have full 256-entry states for
 * the first byte of a UTF-8 character, but only 64-entry states
 * for continuation bytes. Space-optimized tables may only be
 * used with source input that has been checked to be
 * structurally- (or stronger interchange-) valid.
 *
 * A property state table has an unsigned one-byte property for
 * each possible UTF-8 character. One-byte character properties
 * are in the state[0] array, while for other lengths the
 * state[0] array gives the next state, which contains the
 * property value for two-byte characters or yet another state
 * for longer ones. The code simply loads the right number of
 * next-state values, then returns the final byte as property
 * value. There are no actions specified in property tables.
 * States are typically shared for multi-byte UTF-8 characters
 * that all have the same property value.
 *
 * A scanning state table has entries that are either a
 * next-state specifier for bytes that are accepted by the
 * scanner, or an exit action for the last byte of each
 * character that is rejected by the scanner.
 *
 * Scanning long strings involves a tight loop that picks up one
 * byte at a time and follows next-state value back to state[0]
 * for each accepted UTF-8 character. Scanning stops at the end
 * of the string or at the first character encountered that has
 * an exit action such as "reject". Timing information is given
 * below.
 *
 * Since so much of Google's text is 7-bit-ASCII values
 * (approximately 94% of the bytes of web documents), the
 * scanning interpreter has two speed optimizations. One checks
 * 8 bytes at a time to see if they are all in the range lo..hi,
 * as specified in constants in the overall statetable object.
 * The check involves ORing together four 4-byte values that
 * overflow into the high bit of some byte when a byte is out of
 * range. For seven-bit-ASCII, lo is 0x20 and hi is 0x7E. This
 * loop is about 8x faster than the one-byte-at-a-time loop.
 *
 * If checking for exit bytes in the 0x00-0x1F and 7F range is
 * unneeded, an even faster loop just looks at the high bits of
 * 8 bytes at once, and is about 1.33x faster than the lo..hi
 * loop.
 *
 * Exit from the scanning routines backs up to the first byte of
 * the rejected character, so the text spanned is always a
 * complete number of UTF-8 characters. The normal scanning exit
 * is at the first rejected character, or at the end of the
 * input text. Scanning also exits on any detected ill-formed
 * character or at a special do-again action built into some
 * exit-optimized tables. The do-again action gets back to the
 * top of the scanning loop to retry eight-byte ASCII scans. It
 * is typically put into state tables after four seven-bit-ASCII
 * characters in a row are seen, to allow restarting the fast
 * scan after some slower processing of multi-byte characters.
 *
 * A replacement state table is similar to a scanning state
 * table but has more extensive actions. The default
 * byte-at-a-time loop copies one byte from source to
 * destination and goes to the next state. The replacement
 * actions overwrite 1-3 bytes of the destination with different
 * bytes, possibly shortening the output by 1 or 2 bytes. The
 * replacement bytes come from within the state table, from
 * dummy states inserted just after any state that contains a
 * replacement action. This gives a quick address calculation for
 * the replacement byte(s) and gives some cache locality.
 *
 * Additional replacement actions use one or two bytes from
 * within dummy states to index a side table of more-extensive
 * replacements. The side table specifies a length of 0..15
 * destination bytes to overwrite and a length of 0..127 bytes
 * to overwrite them with, plus the actual replacement bytes.
 *
 * This side table uses one extra bit to specify a pair of
 * replacements, the first to be used in an HTML context and the
 * second to be used in a plaintext context. This allows
 * replacements that are spelled with "&lt;" in the former
 * context and "<" in the latter.
 *
 * The side table also uses an extra bit to specify a non-zero
 * next state after a replacement. This allows a combination
 * replacement and state change, used to implement a limited
 * version of the Boyer-Moore algorithm for multi-character
 * replacement without backtracking. This is useful when there
 * are overlapping replacements, such as ch => x and also c =>
 * y, the latter to be used only if the character after c is not
 * h. in this case, the state[0] table's entry for c would
 * change c to y and also have a next-state of say n, and the
 * state[n] entry for h would specify a replacement of the two
 * bytes yh by x. No backtracking is needed.
 *
 * A replacement table may also include the exit actions of a
 * scanning state table, so some character sequences can
 * terminate early.
 *
 * During replacement, an optional data structure called an
 * offset map can be updated to reflect each change in length
 * between source and destination. This offset map can later be
 * used to map destination-string offsets to corresponding
 * source-string offsets or vice versa.
 *
 * The routines below also have variants in which state-table
 * entries are all two bytes instead of one byte. This allows
 * tables with more than 240 total states, but takes up twice as
 * much space per state.
 *
**/

// Return true if current Tbl pointer is within state0 range
// Note that unsigned compare checks both ends of range simultaneously
static inline bool InStateZero(const UTF8ScanObj* st, const uint8_t* Tbl) {
  const uint8_t* Tbl0 = &st->state_table[st->state0];
  return (static_cast<uint32_t>(Tbl - Tbl0) < st->state0_size);
}

// BigOneByte versions are needed for tables > 240 states, but most
// won't need the TwoByte versions.
// Internally, to next-to-last offset is multiplied by 16 and the last
// offset is relative instead of absolute.
// Look up property of one UTF-8 character and advance over it
// Return 0 if input length is zero
// Return 0 and advance one byte if input is ill-formed
uint8_t UTF8GenericPropertyBigOneByte(const UTF8PropObj* st,
                          const uint8_t** src,
                          int* srclen) {
  if (*srclen <= 0) {
    return 0;
  }

  const uint8_t* lsrc = *src;
  const uint8_t* Tbl_0 = &st->state_table[st->state0];
  const uint8_t* Tbl = Tbl_0;
  int e;
  int eshift = st->entry_shift;

  // Short series of tests faster than switch, optimizes 7-bit ASCII
  unsigned char c = lsrc[0];
  if (static_cast<signed char>(c) >= 0) {           // one byte
    e = Tbl[c];
    *src += 1;
    *srclen -= 1;
  } else if (((c & 0xe0) == 0xc0) && (*srclen >= 2)) {     // two bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[1]];
    *src += 2;
    *srclen -= 2;
  } else if (((c & 0xf0) == 0xe0) && (*srclen >= 3)) {     // three bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << (eshift + 4)];  // 16x the range
    e = (reinterpret_cast<const int8_t*>(Tbl))[lsrc[1]];
    Tbl = &Tbl[e << eshift];          // Relative +/-
    e = Tbl[lsrc[2]];
    *src += 3;
    *srclen -= 3;
  }else if (((c & 0xf8) == 0xf0) && (*srclen >= 4)) {     // four bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[1]];
    Tbl = &Tbl_0[e << (eshift + 4)];  // 16x the range
    e = (reinterpret_cast<const int8_t*>(Tbl))[lsrc[2]];
    Tbl = &Tbl[e << eshift];          // Relative +/-
    e = Tbl[lsrc[3]];
    *src += 4;
    *srclen -= 4;
  } else {                                                // Ill-formed
    e = 0;
    *src += 1;
    *srclen -= 1;
  }
  return e;
}



// TwoByte versions are needed for tables > 240 states
// Look up property of one UTF-8 character and advance over it
// Return 0 if input length is zero
// Return 0 and advance one byte if input is ill-formed
uint8_t UTF8GenericPropertyTwoByte(const UTF8PropObj_2* st,
                          const uint8_t** src,
                          int* srclen) {
  if (*srclen <= 0) {
    return 0;
  }

  const uint8_t* lsrc = *src;
  const unsigned short* Tbl_0 = &st->state_table[st->state0];
  const unsigned short* Tbl = Tbl_0;
  int e;
  int eshift = st->entry_shift;

  // Short series of tests faster than switch, optimizes 7-bit ASCII
  unsigned char c = lsrc[0];
  if (static_cast<signed char>(c) >= 0) {           // one byte
    e = Tbl[c];
    *src += 1;
    *srclen -= 1;
  } else if (((c & 0xe0) == 0xc0) && (*srclen >= 2)) {     // two bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[1]];
    *src += 2;
    *srclen -= 2;
  } else if (((c & 0xf0) == 0xe0) && (*srclen >= 3)) {     // three bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[1]];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[2]];
    *src += 3;
    *srclen -= 3;
  }else if (((c & 0xf8) == 0xf0) && (*srclen >= 4)) {     // four bytes
    e = Tbl[c];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[1]];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[2]];
    Tbl = &Tbl_0[e << eshift];
    e = Tbl[lsrc[3]];
    *src += 4;
    *srclen -= 4;
  } else {                                                // Ill-formed
    e = 0;
    *src += 1;
    *srclen -= 1;
  }
  return e;
}



// Approximate speeds on 2.8 GHz Pentium 4:
//   GenericScan 1-byte loop           300 MB/sec *
//   GenericScan 4-byte loop          1200 MB/sec
//   GenericScan 8-byte loop          2400 MB/sec *
//   GenericScanFastAscii 4-byte loop 3000 MB/sec
//   GenericScanFastAscii 8-byte loop 3200 MB/sec *
//
// * Implemented below. FastAscii loop is memory-bandwidth constrained.

// Scan a UTF-8 stringpiece based on state table.
// Always scan complete UTF-8 characters
// Set number of bytes scanned. Return reason for exiting
int UTF8GenericScan(const UTF8ScanObj* st,
                    const StringPiece& str,
                    int* bytes_consumed) {
  int eshift = st->entry_shift;       // 6 (space optimized) or 8
  // int nEntries = (1 << eshift);       // 64 or 256 entries per state

  const uint8_t* isrc =
    reinterpret_cast<const uint8_t*>(str.data());
  const uint8_t* src = isrc;
  const int len = str.length();
  const uint8_t* srclimit = isrc + len;
  const uint8_t* srclimit8 = srclimit - 7;
  *bytes_consumed = 0;
  if (len == 0) return kExitOK;

  const uint8_t* Tbl_0 = &st->state_table[st->state0];

DoAgain:
  // Do state-table scan
  int e = 0;
  uint8_t c;

  // Do fast for groups of 8 identity bytes.
  // This covers a lot of 7-bit ASCII ~8x faster than the 1-byte loop,
  // including slowing slightly on cr/lf/ht
  //----------------------------
  const uint8_t* Tbl2 = &st->fast_state[0];
  uint32_t losub = st->losub;
  uint32_t hiadd = st->hiadd;
  while (src < srclimit8) {
    const uint32_t* src32 = reinterpret_cast<const uint32_t *>(src);
    uint32_t s0123 = UNALIGNED_LOAD32(&src32[0]);
    uint32_t s4567 = UNALIGNED_LOAD32(&src32[1]);
    src += 8;
    // This is a fast range check for all bytes in [lowsub..0x80-hiadd)
    uint32_t temp = (s0123 - losub) | (s0123 + hiadd) |
                  (s4567 - losub) | (s4567 + hiadd);
    if ((temp & 0x80808080) != 0) {
      // We typically end up here on cr/lf/ht; src was incremented
      int e0123 = (Tbl2[src[-8]] | Tbl2[src[-7]]) |
                  (Tbl2[src[-6]] | Tbl2[src[-5]]);
      if (e0123 != 0) {src -= 8; break;}    // Exit on Non-interchange
      e0123 = (Tbl2[src[-4]] | Tbl2[src[-3]]) |
              (Tbl2[src[-2]] | Tbl2[src[-1]]);
      if (e0123 != 0) {src -= 4; break;}    // Exit on Non-interchange
      // Else OK, go around again
    }
  }
  //----------------------------

  // Byte-at-a-time scan
  //----------------------------
  const uint8_t* Tbl = Tbl_0;
  while (src < srclimit) {
    c = *src;
    e = Tbl[c];
    src++;
    if (e >= kExitIllegalStructure) {break;}
    Tbl = &Tbl_0[e << eshift];
  }
  //----------------------------


  // Exit possibilities:
  //  Some exit code, !state0, back up over last char
  //  Some exit code, state0, back up one byte exactly
  //  source consumed, !state0, back up over partial char
  //  source consumed, state0, exit OK
  // For illegal byte in state0, avoid backup up over PREVIOUS char
  // For truncated last char, back up to beginning of it

  if (e >= kExitIllegalStructure) {
    // Back up over exactly one byte of rejected/illegal UTF-8 character
    src--;
    // Back up more if needed
    if (!InStateZero(st, Tbl)) {
      do {src--;} while ((src > isrc) && ((src[0] & 0xc0) == 0x80));
    }
  } else if (!InStateZero(st, Tbl)) {
    // Back up over truncated UTF-8 character
    e = kExitIllegalStructure;
    do {src--;} while ((src > isrc) && ((src[0] & 0xc0) == 0x80));
  } else {
    // Normal termination, source fully consumed
    e = kExitOK;
  }

  if (e == kExitDoAgain) {
    // Loop back up to the fast scan
    goto DoAgain;
  }

  *bytes_consumed = src - isrc;
  return e;
}


// Scan a UTF-8 stringpiece based on state table, copying to output stringpiece
//   and doing text replacements.
// DO NOT CALL DIRECTLY. Use UTF8GenericReplace() below
//   Needs caller to loop on kExitDoAgain
static int UTF8GenericReplaceInternal(const UTF8ReplaceObj* st,
                    const StringPiece& istr,
                    StringPiece& ostr,
                    int* bytes_consumed,
                    int* bytes_filled,
                    int* chars_changed) {
  int eshift = st->entry_shift;
  int nEntries = (1 << eshift);       // 64 or 256 entries per state
  const uint8_t* isrc = reinterpret_cast<const uint8_t*>(istr.data());
  const int ilen = istr.length();
  const uint8_t* src = isrc;
  const uint8_t* srclimit = src + ilen;
  *bytes_consumed = 0;
  *bytes_filled = 0;
  *chars_changed = 0;

  const uint8_t* odst = reinterpret_cast<const uint8_t*>(ostr.data());
  const int olen = ostr.length();
  uint8_t* dst = const_cast<uint8_t*>(odst);
  uint8_t* dstlimit = dst + olen;

  int total_changed = 0;

  // Invariant condition during replacements:
  //  remaining dst size >= remaining src size
  if ((dstlimit - dst) < (srclimit - src)) {
    return kExitDstSpaceFull;
  }
  const uint8_t* Tbl_0 = &st->state_table[st->state0];

 Do_state_table:
  // Do state-table scan, copying as we go
  const uint8_t* Tbl = Tbl_0;
  int e = 0;
  uint8_t c = 0;

 Do_state_table_newe:

  //----------------------------
  while (src < srclimit) {
    c = *src;
    e = Tbl[c];
    *dst = c;
    src++;
    dst++;
    if (e >= kExitIllegalStructure) {break;}
    Tbl = &Tbl_0[e << eshift];
  }
  //----------------------------

  // Exit possibilities:
  //  Replacement code, do the replacement and loop
  //  Some other exit code, state0, back up one byte exactly
  //  Some other exit code, !state0, back up over last char
  //  source consumed, state0, exit OK
  //  source consumed, !state0, back up over partial char
  // For illegal byte in state0, avoid backup up over PREVIOUS char
  // For truncated last char, back up to beginning of it

  if (e >= kExitIllegalStructure) {
    // Switch on exit code; most loop back to top
    int offset = 0;
    switch (e) {
    // These all make the output string the same size or shorter
    // No checking needed
    case kExitReplace31:    // del 2, add 1 bytes to change
      dst -= 2;
      dst[-1] = (unsigned char)Tbl[c + (nEntries * 1)];
      total_changed++;
      goto Do_state_table;
    case kExitReplace32:    // del 3, add 2 bytes to change
      dst--;
      dst[-2] = (unsigned char)Tbl[c + (nEntries * 2)];
      dst[-1] = (unsigned char)Tbl[c + (nEntries * 1)];
      total_changed++;
      goto Do_state_table;
    case kExitReplace21:    // del 2, add 1 bytes to change
      dst--;
      dst[-1] = (unsigned char)Tbl[c + (nEntries * 1)];
      total_changed++;
      goto Do_state_table;
    case kExitReplace3:    // update 3 bytes to change
      dst[-3] = (unsigned char)Tbl[c + (nEntries * 3)];
      // Fall into next case
    case kExitReplace2:    // update 2 bytes to change
      dst[-2] = (unsigned char)Tbl[c + (nEntries * 2)];
      // Fall into next case
    case kExitReplace1:    // update 1 byte to change
      dst[-1] = (unsigned char)Tbl[c + (nEntries * 1)];
      total_changed++;
      goto Do_state_table;
    case kExitReplace1S0:     // update 1 byte to change, 256-entry state
      dst[-1] = (unsigned char)Tbl[c + (256 * 1)];
      total_changed++;
      goto Do_state_table;
    // These can make the output string longer than the input
    case kExitReplaceOffset2:
      if ((nEntries != 256) && InStateZero(st, Tbl)) {
        // For space-optimized table, we need multiples of 256 bytes
        // in state0 and multiples of nEntries in other states
        offset += ((unsigned char)Tbl[c + (256 * 2)] << 8);
      } else {
        offset += ((unsigned char)Tbl[c + (nEntries * 2)] << 8);
      }
      // Fall into next case
    case kExitSpecial:      // Apply special fixups [read: hacks]
    case kExitReplaceOffset1:
      if ((nEntries != 256) && InStateZero(st, Tbl)) {
        // For space-optimized table, we need multiples of 256 bytes
        // in state0 and multiples of nEntries in other states
        offset += (unsigned char)Tbl[c + (256 * 1)];
      } else {
        offset += (unsigned char)Tbl[c + (nEntries * 1)];
      }
      {
        const RemapEntry* re = &st->remap_base[offset];
        int del_len = re->delete_bytes & ~kReplaceAndResumeFlag;
        int add_len = re->add_bytes & ~kHtmlPlaintextFlag;

        int string_offset = re->bytes_offset;
        // After the replacement, need (dstlimit - newdst) >= (srclimit - src)
        uint8_t* newdst = dst - del_len + add_len;
        if ((dstlimit - newdst) < (srclimit - src)) {
          // Won't fit; don't do the replacement. Caller may realloc and retry
          e = kExitDstSpaceFull;
          break;    // exit, backing up over this char for later retry
        }
        dst -= del_len;
        memcpy(dst, &st->remap_string[string_offset], add_len);
        dst += add_len;
        total_changed++;
        if (re->delete_bytes & kReplaceAndResumeFlag) {
          // There is a non-zero  target state at the end of the
          // replacement string
          e = st->remap_string[string_offset + add_len];
          Tbl = &Tbl_0[e << eshift];
          goto Do_state_table_newe;
        }
      }
      if (e == kExitRejectAlt) {break;}
      goto Do_state_table;

    case kExitIllegalStructure:   // structurally illegal byte; quit
    case kExitReject:             // NUL or illegal code encountered; quit
    case kExitRejectAlt:          // Apply replacement, then exit
    default:                      // and all other exits
      break;
    }   // End switch (e)

    // Exit possibilities:
    //  Some other exit code, state0, back up one byte exactly
    //  Some other exit code, !state0, back up over last char

    // Back up over exactly one byte of rejected/illegal UTF-8 character
    src--;
    dst--;
    // Back up more if needed
    if (!InStateZero(st, Tbl)) {
      do {src--;dst--;} while ((src > isrc) && ((src[0] & 0xc0) == 0x80));
    }
  } else if (!InStateZero(st, Tbl)) {
    // src >= srclimit, !state0
    // Back up over truncated UTF-8 character
    e = kExitIllegalStructure;
    do {src--; dst--;} while ((src > isrc) && ((src[0] & 0xc0) == 0x80));
  } else {
    // src >= srclimit, state0
    // Normal termination, source fully consumed
    e = kExitOK;
  }

  // Possible return values here:
  //  kExitDstSpaceFull         caller may realloc and retry from middle
  //  kExitIllegalStructure     caller my overwrite/truncate
  //  kExitOK                   all done and happy
  //  kExitReject               caller may overwrite/truncate
  //  kExitDoAgain              LOOP NOT DONE; caller must retry from middle
  //                            (may do fast ASCII loop first)
  //  kExitPlaceholder          -unused-
  //  kExitNone                 -unused-
  *bytes_consumed = src - isrc;
  *bytes_filled = dst - odst;
  *chars_changed = total_changed;
  return e;
}

// Scan a UTF-8 stringpiece based on state table, copying to output stringpiece
//   and doing text replacements.
// Always scan complete UTF-8 characters
// Set number of bytes consumed from input, number filled to output.
// Return reason for exiting
int UTF8GenericReplace(const UTF8ReplaceObj* st,
                    const StringPiece& istr,
                    StringPiece& ostr,
                    int* bytes_consumed,
                    int* bytes_filled,
                    int* chars_changed) {
  StringPiece local_istr(istr.data(), istr.length());
  StringPiece local_ostr(ostr.data(), ostr.length());
  int total_consumed = 0;
  int total_filled = 0;
  int total_changed = 0;
  int local_bytes_consumed, local_bytes_filled, local_chars_changed;
  int e;
  do {
    e = UTF8GenericReplaceInternal(st,
                    local_istr, local_ostr,
                    &local_bytes_consumed, &local_bytes_filled,
                    &local_chars_changed);
    local_istr.remove_prefix(local_bytes_consumed);
    local_ostr.remove_prefix(local_bytes_filled);
    total_consumed += local_bytes_consumed;
    total_filled += local_bytes_filled;
    total_changed += local_chars_changed;
  } while ( e == kExitDoAgain );
  *bytes_consumed = total_consumed;
  *bytes_filled = total_filled;
  *chars_changed = total_changed;
  return e;
}

}       // End namespace CLD2
