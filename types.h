/*
 * Copyright 2017 Sam Thorogood. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#include <stdint.h>

#ifndef _TYPES_H
#define _TYPES_H

#define ERROR__INTERNAL -1  // internal error
#define ERROR__STACK    -2  // stack didn't balance
#define ERROR__VALUE    -3  // ambiguous slash (internal error)
#define ERROR__ASSERT   -4

#define __STACK_SIZE      256  // stack size used by token
#define __STACK_SIZE_BITS 8    // bits needed for __STACK_SIZE

typedef struct {
  char *p;
  int len;
  int line_no;
  uint8_t type : 5;
  uint8_t mark : 3;
  uint32_t hash;
} token;

// empty: will not contain text
#define TOKEN_EOF       0

// fixed: will always be the same, or in the same set
#define TOKEN_EXEC      1   // block '{' or blank for statement
#define TOKEN_SEMICOLON 2   // might be blank for ASI
#define TOKEN_OP        4   // can include 'in', 'instanceof'
#define TOKEN_ARROW     5
#define TOKEN_COLON     6   // used in label or dict
#define TOKEN_DICT      7   // dict-like {
#define TOKEN_ARRAY     8
#define TOKEN_PAREN     9
#define TOKEN_T_BRACE   10  // '${' within template literal
#define TOKEN_TERNARY   11  // starts ternary block, "? ... :"
#define TOKEN_CLOSE     12  // '}', ']', ')', ':' or blank for statement close

// variable: could be anything
#define TOKEN_COMMENT   13
#define TOKEN_STRING    14
#define TOKEN_REGEXP    15  // literal "/foo/", not "new RegExp('foo')"
#define TOKEN_NUMBER    16
#define TOKEN_SYMBOL    17
#define TOKEN_KEYWORD   18
#define TOKEN_LABEL     19  // to the left of a ':', e.g. 'foo:'

#define TOKEN_START     25
#define TOKEN_ATTACH    26
#define TOKEN_MORE      27

// internal/ambiguous tokens
#define TOKEN_TOP       28  // never reported, top of function or program
#define TOKEN_BRACE     29  // ambig brace
#define TOKEN_LIT       30  // symbol, keyword or label
#define TOKEN_SLASH     31  // ambigous slash that is op or regexp

// special marks
#define MARK_RESOLVE    2   // resolving a prior lit (always "async")

#endif//_TYPES_H

