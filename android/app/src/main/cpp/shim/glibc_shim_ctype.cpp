// shim/glibc_shim_ctype.cpp — glibc-format ctype tables.
//
// The guest calls __ctype_b_loc()/__ctype_tolower_loc()/__ctype_toupper_loc(),
// which return pointers to pointer-into-table in glibc format: tables of 384
// entries indexed as table[c + 128] (c in [-128, 255]). Bionic has different
// tables, so we build our own in glibc format at startup.
//
// Flag bits (glibc ctype.h, stable ABI for decades):
//   _ISupper  (1 << 8)   _ISlower  (1 << 9)   _ISalpha  (1 << 10)
//   _ISdigit  (1 << 11)  _ISxdigit (1 << 12)  _ISspace  (1 << 13)
//   _ISprint  (1 << 14)  _ISgraph  (1 << 15)  _IScntrl  (1 << 0)
//   _ISpunct  (1 << 1)
// The tolower/toupper tables are int32 translations (identity unless alphabetic).
#include "shim_common.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

namespace {

int16_t g_ctype_b[384];
int32_t g_ctype_tolower[384];
int32_t g_ctype_toupper[384];
const int16_t* g_ctype_b_ptr = nullptr;
const int32_t* g_ctype_tolower_ptr = nullptr;
const int32_t* g_ctype_toupper_ptr = nullptr;

constexpr int16_t kUpper = 1 << 8, kLower = 1 << 9, kAlpha = 1 << 10,
                  kDigit = 1 << 11, kXdigit = 1 << 12, kSpace = 1 << 13,
                  kPrint = 1 << 14, kGraph = 1 << 15, kCntrl = 1 << 0,
                  kPunct = 1 << 1;

__attribute__((constructor(101))) static void build_ctype_tables() {
    for (int c = -128; c < 256; ++c) {
        int idx = c + 128;
        int16_t f = 0;
        int32_t lo = c, up = c;
        if (c >= 0 && c < 256) {
            if (isupper(c)) f |= kUpper | kAlpha | kGraph | kPrint;
            if (islower(c)) f |= kLower | kAlpha | kGraph | kPrint;
            if (isdigit(c)) f |= kDigit | kGraph | kPrint;
            if (isxdigit(c)) f |= kXdigit | kGraph | kPrint;
            if (isspace(c)) f |= kSpace;
            if (iscntrl(c)) f |= kCntrl;
            if (ispunct(c)) f |= kPunct | kGraph | kPrint;
            if (c == ' ') f |= kPrint | kSpace;
            if (isalpha(c)) {
                lo = tolower(c);
                up = toupper(c);
            }
        }
        g_ctype_b[idx] = f;
        g_ctype_tolower[idx] = lo;
        g_ctype_toupper[idx] = up;
    }
    // glibc returns a pointer INTO the table (table + 128) so negative indexes work.
    g_ctype_b_ptr = g_ctype_b + 128;
    g_ctype_tolower_ptr = g_ctype_tolower + 128;
    g_ctype_toupper_ptr = g_ctype_toupper + 128;
}

} // namespace

extern "C" {

const int16_t** guest___ctype_b_loc(void) {
    return (const int16_t**)&g_ctype_b_ptr;
}

const int32_t** guest___ctype_tolower_loc(void) {
    return (const int32_t**)&g_ctype_tolower_ptr;
}

const int32_t** guest___ctype_toupper_loc(void) {
    return (const int32_t**)&g_ctype_toupper_ptr;
}

} // extern "C"
