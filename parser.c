
#include <string.h>
#include "parser.h"
#include "tokens/lit.h"

#define SSTACK__EXPR     0
#define SSTACK__CONTROL  1  // control group e.g. "for (...)"
#define SSTACK__BLOCK    2  // block execution context
#define SSTACK__DICT     3  // within regular dict "{}"
#define SSTACK__FUNC     4  // expects upcoming "name () {}"
#define SSTACK__CLASS    5  // expects "extends X"? "{}"
#define SSTACK__MODULE   6  // state machine for import/export defs
#define SSTACK__ASYNC    7  // async arrow function

#ifdef DEBUG
#include <stdio.h>
#define debugf(...) fprintf(stderr, __VA_ARGS__)
#else
#define debugf (void)sizeof
#endif


typedef struct {
  token prev;           // previous token
  uint32_t start;       // hash of stype start (set only for some stypes)
  uint8_t stype : 3;    // stack type
  uint8_t context : 3;  // current execution context (strict, async, generator)
} sstack;


typedef struct {
  tokendef *td;
  token *next;  // convenience
  token tok;
  int is_module;

  prsr_callback cb;
  void *arg;
  int prev_line_no;

  sstack *curr;
  sstack stack[__STACK_SIZE];
} simpledef;


static sstack *stack_inc(simpledef *sd, uint8_t stype) {
  // TODO: check bounds
  ++sd->curr;
  bzero(sd->curr, sizeof(sstack));
  sd->curr->stype = stype;
  sd->curr->context = (sd->curr - 1)->context;  // copy context
  return sd->curr;
}


// stores a virtual token in the stream, and yields it before the current token
static void yield_virt(simpledef *sd, int type) {
  token *t = &(sd->curr->prev);
  bzero(t, sizeof(token));

  t->line_no = sd->prev_line_no;
  t->type = type;

  sd->cb(sd->arg, t);
}


static void yield_virt_skip(simpledef *sd, int type) {
  token t;
  bzero(&t, sizeof(token));

  t.line_no = sd->prev_line_no;
  t.type = type;

  sd->cb(sd->arg, &t);
}


// optionally yields ASI for restrict, assumes sd->curr->prev is the restricted keyword
// pops to nearby block
static int yield_restrict_asi(simpledef *sd) {
  int line_no = sd->curr->prev.line_no;

  if (line_no == sd->tok.line_no && sd->tok.type != TOKEN_CLOSE) {
    return 0;  // not new line, not close token
  }

  sstack *c = sd->curr;
  if (c->stype == SSTACK__BLOCK) {
    // ok
  } else if (c->stype == SSTACK__EXPR && (c - 1)->stype == SSTACK__BLOCK) {
    --sd->curr;
  } else {
    return 0;
  }

  yield_virt(sd, TOKEN_SEMICOLON);
  return 1;
}


// places the next useful token in sd->tok, yielding previous current
static int skip_walk(simpledef *sd, int has_value) {
  if (sd->tok.p) {
    sd->prev_line_no = sd->tok.line_no;
    sd->cb(sd->arg, &(sd->tok));
  }
  for (;;) {
    // prsr_next_token can reveal comments, loop until over them
    int out = prsr_next_token(sd->td, &(sd->tok), has_value);
    if (out || sd->tok.type != TOKEN_COMMENT) {
      return out;
    }
    sd->cb(sd->arg, &(sd->tok));
  }
}


// records/yields the current token, places the next useful token
static int record_walk(simpledef *sd, int has_value) {
  sd->curr->prev = sd->tok;
  return skip_walk(sd, has_value);
}


static int is_optional_keyword(uint32_t hash, uint8_t context) {
  if (context & CONTEXT__ASYNC && hash == LIT_AWAIT) {
    return 1;
  } else if (context & (CONTEXT__GENERATOR | CONTEXT__STRICT) && hash == LIT_YIELD) {
    // yield is invalid outside a generator in strict mode, but it's a keyword
    return 1;
  }
  return 0;
}


static int is_always_keyword(uint32_t hash, uint8_t context) {
  return (hash & _MASK_KEYWORD) ||
      ((context & CONTEXT__STRICT) && (hash & _MASK_STRICT_KEYWORD));
}


static int is_label(token *t, uint8_t context) {
  if (t->type != TOKEN_LIT) {
    return 0;
  } else if (t->type == TOKEN_LABEL) {
    return 1;
  }
  return !is_always_keyword(t->hash, context) && !is_optional_keyword(t->hash, context);
}


static int is_valid_name(uint32_t hash, uint8_t context) {
  uint32_t mask = _MASK_KEYWORD | _MASK_MASQUERADE;
  if (context & CONTEXT__STRICT) {
    mask |= _MASK_STRICT_KEYWORD;
  }

  if ((context & CONTEXT__ASYNC) && hash == LIT_AWAIT) {
    // await is a keyword inside async function
    return 0;
  }

  if ((context & CONTEXT__GENERATOR) && hash == LIT_YIELD) {
    // yield is a keyword inside generator function
    return 0;
  }

  return !(hash & mask);
}


static int is_unary(uint32_t hash, uint8_t context) {
  // check if we're also a keyword, to avoid matching 'await' and 'yield' by default
  uint32_t mask = _MASK_UNARY_OP | _MASK_KEYWORD;
  return ((hash & mask) == mask) || is_optional_keyword(hash, context);
}


// matches any current function decl/stmt
static int match_function(simpledef *sd) {
  if (sd->tok.hash == LIT_ASYNC) {
    if (sd->next->hash != LIT_FUNCTION) {
      return -1;
    }
    // otherwise fine
  } else if (sd->tok.hash != LIT_FUNCTION) {
    return -1;
  }

  uint8_t context = (sd->curr->context & CONTEXT__STRICT);
  if (sd->tok.hash == LIT_ASYNC) {
    context |= CONTEXT__ASYNC;
    sd->tok.type = TOKEN_KEYWORD;
    skip_walk(sd, -1);  // consume "async"
  }
  sd->tok.type = TOKEN_KEYWORD;
  record_walk(sd, -1);  // consume "function"

  // optionally consume generator star
  if (sd->tok.hash == MISC_STAR) {
    skip_walk(sd, 0);
    context |= CONTEXT__GENERATOR;
  }

  // nb. does NOT consume name
  return context;
}


static int match_class(simpledef *sd) {
  if (sd->tok.hash != LIT_CLASS) {
    return -1;
  }
  sd->tok.type = TOKEN_KEYWORD;
  record_walk(sd, 0);  // consume "class"

  // optionally consume class name if not "extends"
  uint32_t h = sd->tok.hash;
  if (h == LIT_EXTENDS || sd->tok.type != TOKEN_LIT) {
    return 0;  // ... if this isn't TOKEN_BRACE, it's invalid, but let stack handler deal
  } else if (!is_valid_name(h, sd->curr->context) || h == LIT_YIELD || h == LIT_LET) {
    // nb. "yield" or "let" are both always invalid, even in non-strict (doesn't apply to function)
    // ... this might actually be a V8 "feature", but it's the same in Firefox, and both actually
    // complain that it's invalid in strict mode even if _not_ in that mode (!)
    sd->tok.type = TOKEN_KEYWORD;  // "class if" is invalid
  } else {
    sd->tok.type = TOKEN_SYMBOL;
  }
  skip_walk(sd, 0);  // consume name even if it's an invalid keyword
  return 0;
}


static int enact_defn(simpledef *sd) {
  // ...match function
  int context = match_function(sd);
  if (context >= 0) {
    stack_inc(sd, SSTACK__FUNC);
    sd->curr->context = context;
    return 1;
  }

  // ... match class
  int class = match_class(sd);
  if (class >= 0) {
    stack_inc(sd, SSTACK__CLASS);
    return 1;
  }

  return 0;
}


// matches a "break foo;" or "continue;", emits ASI if required
static int match_label_keyword(simpledef *sd) {
  if (sd->tok.hash != LIT_BREAK && sd->tok.hash != LIT_CONTINUE) {
    return -1;
  }

  int line_no = sd->tok.line_no;
  sd->tok.type = TOKEN_KEYWORD;
  record_walk(sd, 0);

  if (sd->tok.line_no == line_no && is_label(&(sd->tok), sd->curr->context)) {
    sd->tok.type = TOKEN_LABEL;
    skip_walk(sd, 0);  // don't consume, so yield_restrict_asi works
  }

  // e.g. "break\n" or "break foo\n"
  if (!yield_restrict_asi(sd) && sd->tok.type == TOKEN_SEMICOLON) {
    skip_walk(sd, -1);  // emit or consume valid semicolon
  }
  return 0;
}


static int is_use_strict(token *t) {
  if (t->type != TOKEN_STRING || t->len != 12) {
    return 0;
  }
  return !memcmp(t->p, "'use strict'", 12) || !memcmp(t->p, "\"use strict\"", 12);
}


// is the next token valuelike for a previous valuelike?
// used directly only for "let" and "await" (at top-level), so doesn't include e.g. paren or array,
// as these would be indexing or calling
static int is_token_valuelike(token *t) {
  switch (t->type) {
    case TOKEN_LIT:
      // _any_ lit is fine (even keywords, even if invalid) except "in" and "instanceof"
      return !(t->hash & _MASK_REL_OP);

    case TOKEN_SYMBOL:
    case TOKEN_NUMBER:
    case TOKEN_STRING:
    case TOKEN_BRACE:
      return 1;

    case TOKEN_OP:
      // https://www.ecma-international.org/ecma-262/9.0/index.html#prod-UnaryExpression
      // FIXME: in Chrome's top-level await support (e.g. in DevTools), this also includes + and -
      return t->hash == MISC_NOT || t->hash == MISC_BITNOT;
  }

  return 0;
}


// is this token valuelike following "of" inside a "for (... of ...)"?
static int is_token_valuelike_keyword(token *t) {
  if (is_token_valuelike(t)) {
    return 1;
  }
  switch (t->type) {
    case TOKEN_PAREN:
    case TOKEN_ARRAY:
    case TOKEN_BRACE:
    case TOKEN_SLASH:
    case TOKEN_REGEXP:
      return 1;
  }
  return 0;
}


// matches var/const/let, optional let based on context
static int match_decl(simpledef *sd) {
  if (!(sd->tok.hash & _MASK_DECL)) {
    return -1;
  }

  // in strict mode, 'let' is always a keyword (well, reserved)
  if (!(sd->curr->context & CONTEXT__STRICT) && sd->tok.hash == LIT_LET) {
    if (!is_token_valuelike(sd->next) && sd->next->type != TOKEN_ARRAY) {
      // ... let[] is declaration, but e.g. "await []" is an index
      return -1;
    }
    // OK: destructuring "let[..]" or "let{..}", and not with "in" or "instanceof" following
  }

  sd->tok.type = TOKEN_KEYWORD;
  return record_walk(sd, 0);
}


static int simple_start_arrowfunc(simpledef *sd, int async) {
#ifdef DEBUG
  if (sd->tok.type != TOKEN_ARROW) {
    debugf("arrowfunc start without TOKEN_ARROW\n");
    return ERROR__ASSERT;
  }
  if (sd->curr->stype != SSTACK__EXPR) {
    debugf("arrowfunc start not inside EXPR\n");
    return ERROR__ASSERT;
  }
#endif

  uint8_t context = (sd->curr->context & CONTEXT__STRICT);
  if (async) {
    context |= CONTEXT__ASYNC;
  }

  if (sd->next->type == TOKEN_BRACE) {
    // the sensible arrow function case, with a proper body
    // e.g. "() => { statements }"
    record_walk(sd, -1);  // consume =>
    sd->tok.type = TOKEN_EXEC;
    record_walk(sd, -1);  // consume {
    stack_inc(sd, SSTACK__BLOCK);
    sd->curr->prev.type = TOKEN_TOP;
  } else {
    // just change statement's context (e.g. () => async () => () => ...)
    record_walk(sd, -1);  // consume =>
    sd->curr->prev.type = TOKEN_EOF;  // pretend statement finished
  }
  sd->curr->context = context;
  return 0;
}


// consumes an expr (always SSTACK__EXPR)
// MUST NOT assume parent is SSTACK__BLOCK, could be anything
static int simple_consume_expr(simpledef *sd) {
  int ptype = sd->curr->prev.type;

  switch (sd->tok.type) {
    case TOKEN_SEMICOLON:
      if ((sd->curr - 1)->stype == SSTACK__BLOCK) {
        --sd->curr;
      } else {
        // invalid
      }
      record_walk(sd, -1);  // semi goes in block
      return 0;

    case TOKEN_ARROW:
      if (ptype != TOKEN_PAREN && ptype != TOKEN_SYMBOL) {
        // not a valid arrow func, treat as op
        return record_walk(sd, -1);
      }
      return simple_start_arrowfunc(sd, 0);

    case TOKEN_EOF:
      if ((sd->curr - 1)->stype != SSTACK__BLOCK) {
        // EOF only closes statement within block
        return 0;
      }
      // fall-through

    case TOKEN_CLOSE:
      --sd->curr;  // always valid to close here (SSTACK__BLOCK catches invalid close)

      switch (sd->curr->stype) {
        case SSTACK__BLOCK: {
          // parent is block, maybe yield ASI but pop either way
          if (ptype) {
            yield_virt(sd, TOKEN_SEMICOLON);
          }
          return 0;
        }

        case SSTACK__ASYNC:
          // we're in "async ()", expect arrow next, but if not, we have value
          skip_walk(sd, 1);
          return 0;

        default:
          // this would be hoisted class/func or control group, not a value after
          if ((sd->curr + 1)->start) {
            // ... had a start token, so walk over close token
            skip_walk(sd, 0);
          } else {
            debugf("handing close to parent stype\n");
            // ... got a close while in expr which isn't in group, let parent handle
            // probably error, e.g. "{ class extends }"
          }
          return 0;

        case SSTACK__EXPR:
          break;
      }

      // value if this places us into a statement/group (not if this was ternary, but caught in token.c)
      skip_walk(sd, sd->curr->stype == SSTACK__EXPR);
      return 0;

    case TOKEN_BRACE:
      if (ptype != TOKEN_OP && !sd->curr->start) {
        // found an invalid brace (not following op, not in group), yield to parent
        int yield = (sd->tok.line_no != sd->curr->prev.line_no &&
            ptype &&
            (sd->curr - 1)->stype == SSTACK__BLOCK);
        --sd->curr;
        if (yield) {
          yield_virt(sd, TOKEN_SEMICOLON);
        }
        debugf("invalid brace in statement, yield to parent\n");
        return 0;
      } else if (sd->curr->start == LIT_EXTENDS && ptype) {
        // ... special-case finding the class decl after an extends block
        --sd->curr;
        return 0;
      }
      sd->tok.type = TOKEN_DICT;
      record_walk(sd, -1);
      stack_inc(sd, SSTACK__DICT);
      return 0;

    case TOKEN_TERNARY:
    case TOKEN_ARRAY:
    case TOKEN_PAREN:
    case TOKEN_T_BRACE:
      record_walk(sd, -1);
      stack_inc(sd, SSTACK__EXPR);
      sd->curr->start = sd->tok.type;
      return 0;

    case TOKEN_LIT:
      if (sd->tok.hash & _MASK_REL_OP) {
        sd->tok.type = TOKEN_OP;
        return record_walk(sd, 0);
      }
      // nb. we catch "await", "delete", "new" etc below
      // fall-through

    case TOKEN_STRING:
      if (ptype == TOKEN_T_BRACE) {
        // if we're a string following ${}, this is part a of a template literal and doesn't have
        // special ASI casing (e.g. '${\n\n}' isn't really causing a newline)
        return record_walk(sd, -1);
      }
      // fall-through

    case TOKEN_REGEXP:
    case TOKEN_NUMBER:
      // basic ASI detection inside statement: value on a new line than before, with value
      if ((sd->curr - 1)->stype == SSTACK__BLOCK &&
          sd->tok.line_no != sd->curr->prev.line_no &&
          ptype &&
          ptype != TOKEN_OP) {
        --sd->curr;
        yield_virt(sd, TOKEN_SEMICOLON);
        return 0;
      }

      if (sd->tok.type == TOKEN_LIT) {
        if (sd->tok.hash) {
          break;  // special lit handling
        }
        // ... no hash, always just a regular value
        sd->tok.type = TOKEN_SYMBOL;
      }
      return record_walk(sd, 1);  // otherwise, just a regular value

    case TOKEN_OP:
      switch (sd->tok.hash) {
        case MISC_COMMA:
          // special-case comma in dict, puts us back on left
          if ((sd->curr - 1)->stype == SSTACK__DICT) {
            --sd->curr;
            return 0;
          }
          // clears context (for arrow async weirdness)
          sd->curr->context = (sd->curr - 1)->context;
          // fall-through

        default:
          return record_walk(sd, -1);

        case MISC_INCDEC:
          break;
      }

      // if this is operating _on_ something in the statement, then don't record it
      if (ptype && ptype != TOKEN_OP) {
        if (sd->tok.line_no == sd->curr->prev.line_no) {
          // ... don't record this, right-side (e.g. "a++")
          debugf("not recording right-side +/--\n");
          return skip_walk(sd, 0);
        }

        // otherwise, it's on a newline (invalid in pure statement, generate ASI otherwise)
        // ... this is a PostfixExpression that disallows LineTerminator
        if ((sd->curr - 1)->stype == SSTACK__BLOCK) {
          yield_virt(sd, TOKEN_SEMICOLON);
          yield_virt(sd, TOKEN_START);
        }
      }
      debugf("got left-side ++/--\n");
      return record_walk(sd, 0);

    case TOKEN_COLON:
      if ((sd->curr - 1)->stype == SSTACK__BLOCK) {
        --sd->curr;  // this catches cases like "case {}:", pretend that was an expr on its own
      } else {
        // does nothing here (invalid)
      }
      return record_walk(sd, -1);

    default:
      debugf("unhandled token=%d `%.*s`\n", sd->tok.type, sd->tok.len, sd->tok.p);
      return ERROR__INTERNAL;
  }

  // match function or class as value
  if (enact_defn(sd)) {
    return 0;
  }

  uint32_t outer_hash = sd->tok.hash;

  // match valid unary ops
  if (is_unary(outer_hash, sd->curr->context)) {
    sd->tok.type = TOKEN_OP;
    record_walk(sd, 0);

    if (sd->curr->prev.hash == LIT_YIELD) {
      // yield is a restricted keyword (this does nothing inside group, but is invalid)
      yield_restrict_asi(sd);
    }
    return 0;
  }

  // match non-async await: this is valid iff it _looks_ like unary op use (e.g. await value).
  // this is a lookahead for value, rather than what we normally do
  if (outer_hash == LIT_AWAIT && is_token_valuelike(sd->next)) {
    // ... to be clear, this is an error, but it IS parsed as a keyword
    sd->tok.type = TOKEN_KEYWORD;
    return record_walk(sd, 0);
  }

  // match curious cases inside "for ("
  sstack *up = (sd->curr - 1);
  if (up->stype == SSTACK__CONTROL && up->start == LIT_FOR) {
    // start of "for (", look for decl (var/let/const) and mark as keyword
    if (!ptype) {
      if (match_decl(sd) >= 0) {
        return 0;
      }
    } else if (outer_hash == LIT_OF && ptype != TOKEN_OP && is_token_valuelike_keyword(sd->next)) {
      // ... find "of" between two value-like things
      sd->tok.type = TOKEN_OP;
      return record_walk(sd, 0);
    }
  }

  // aggressive keyword match inside statement
  if (is_always_keyword(outer_hash, sd->curr->context)) {
    if (up->stype == SSTACK__BLOCK && ptype && sd->tok.line_no != sd->curr->prev.line_no) {
      // if a keyword on a new line would make an invalid statement, restart with it
      --sd->curr;
      yield_virt(sd, TOKEN_SEMICOLON);
      return 0;
    }
    // ... otherwise, it's an invalid keyword
    sd->tok.type = TOKEN_KEYWORD;
    return record_walk(sd, 0);
  }

  // look for async arrow function
  if (outer_hash == LIT_ASYNC) {
    switch (ptype) {
      case TOKEN_OP:
        if (sd->curr->prev.hash != MISC_EQUALS) {
          // ... "1 + async () => x" is invalid, only "... = async () =>" is fine
          break;
        }
        // fall-through

      case TOKEN_EOF:
        switch (sd->next->type) {
          case TOKEN_LIT:
            sd->tok.type = TOKEN_KEYWORD;  // "async foo" always makes keyword
            // fall-through

          case TOKEN_PAREN:
            // consume and push SSTACK__ASYNC even if we already know keyword
            // ... otherwise this explicitly remains as LIT until resolved
            record_walk(sd, -1);
            stack_inc(sd, SSTACK__ASYNC);
            return 0;
        }
    }

    sd->tok.type = TOKEN_SYMBOL;
    return record_walk(sd, 1);
  }

  // if nothing else known, treat as symbol
  if (sd->tok.type == TOKEN_LIT) {
    sd->tok.type = TOKEN_SYMBOL;
  }
  return record_walk(sd, 1);
}


static int simple_consume(simpledef *sd) {
  switch (sd->curr->stype) {
    // async arrow function state
    case SSTACK__ASYNC:
      switch (sd->curr->prev.type) {
        default:
          debugf("invalid type in SSTACK__ASYNC: %d\n", sd->curr->prev.type);
          --sd->curr;
          return 0;

        case TOKEN_EOF:
          // start of ambig, insert expr
          if (sd->tok.type == TOKEN_PAREN) {
            record_walk(sd, -1);
            stack_inc(sd, SSTACK__EXPR);
            sd->curr->start = TOKEN_PAREN;
            return 0;
          } else if (sd->tok.type != TOKEN_LIT) {
            return ERROR__INTERNAL;
          }

          // set type of 'x' in "async x =>": keywords are invalid, but allow anyway
          sd->tok.type = is_always_keyword(sd->tok.hash, sd->curr->context) ? TOKEN_KEYWORD : TOKEN_SYMBOL;
          record_walk(sd, 0);
          break;

        case TOKEN_PAREN: {
          // end of ambig, check whether arrow exists
          token *yield = &((sd->curr - 1)->prev);
          yield->type = (sd->tok.type == TOKEN_ARROW ? TOKEN_KEYWORD : TOKEN_SYMBOL);
          yield->mark = MARK_RESOLVE;
          sd->cb(sd->arg, yield);
          break;
        }
      }

      if (sd->tok.type != TOKEN_ARROW) {
        debugf("async starter without arrow, ignoring (%d)\n", sd->tok.type);
        --sd->curr;
        return 0;
      }

      --sd->curr;  // pop SSTACK__ASYNC
      return simple_start_arrowfunc(sd, 1);

    // import state
    case SSTACK__MODULE: {
      int line_no = sd->tok.line_no;

      switch (sd->tok.type) {
        case TOKEN_BRACE:
          sd->tok.type = TOKEN_DICT;
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__MODULE);
          return 0;

        // unexpected, but handle anyway
        case TOKEN_T_BRACE:
        case TOKEN_PAREN:
        case TOKEN_ARRAY:
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__EXPR);
          sd->curr->start = sd->tok.type;
          return 0;

        case TOKEN_STRING:
          if (!sd->curr->prev.type) {
            goto finalize_module;
          }
          goto abandon_module;

        case TOKEN_LIT:
          if ((sd->curr - 1)->stype != SSTACK__MODULE &&
              sd->curr->prev.type == TOKEN_SYMBOL &&
              sd->tok.hash == LIT_FROM) {
            goto finalize_module;
          }
          break;

        case TOKEN_CLOSE:
          if ((sd->curr - 1)->stype != SSTACK__MODULE) {
            debugf("module internal error\n");
            return ERROR__INTERNAL;  // impossible, we're at top-level
          }
          skip_walk(sd, 0);
          --sd->curr;  // close inner

          if ((sd->curr - 1)->stype == SSTACK__MODULE) {
            return 0;  // invalid several descendant case
          }
finalize_module:
          --sd->curr;  // close outer

          if (sd->tok.hash == LIT_FROM) {
            // ... inner {} must have trailer "from './path'"
            sd->tok.type = TOKEN_KEYWORD;
            record_walk(sd, -1);
          }
          if (sd->tok.type == TOKEN_STRING) {
            // this ends import, ensure "... 'foo' /123/" is regexp
            prsr_close_op_next(sd->td);
            record_walk(sd, 0);
          }

          if (sd->tok.type == TOKEN_SEMICOLON) {
            record_walk(sd, -1);
          } else if (sd->tok.line_no != line_no) {
            yield_virt(sd, TOKEN_SEMICOLON);
          }
          return 0;

        case TOKEN_OP:
          if (sd->tok.hash == MISC_STAR) {
            sd->tok.type = TOKEN_SYMBOL;  // pretend this is symbol
            return record_walk(sd, -1);
          } else if (sd->tok.hash == MISC_COMMA) {
            return record_walk(sd, -1);
          }
          // fall-through

        default:
abandon_module:
          if ((sd->curr - 1)->stype != SSTACK__MODULE) {
            debugf("abandoning module for reasons: %d\n", sd->tok.type);
            --sd->curr;
            return 0;  // not inside submodule, just give up
          }
          return record_walk(sd, 0);
      }

      // consume "as" as a keyword if it follows a symbol
      if (sd->curr->prev.type == TOKEN_SYMBOL && sd->tok.hash == LIT_AS) {
        sd->tok.type = TOKEN_KEYWORD;
        return record_walk(sd, 0);
      }

      // otherwise just mask as symbol (we try to place into global namespace always, even for
      // "bad" tokens)
      sd->tok.type = TOKEN_SYMBOL;
      return record_walk(sd, 0);
    }

    // dict state (left)
    case SSTACK__DICT: {
      uint8_t context = 0;

      // search for function
      // ... look for 'static' without '(' next
      if (sd->td->next.type != TOKEN_PAREN &&
          sd->tok.hash == LIT_STATIC) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);
      }

      // ... look for 'async' without '(' next
      if (sd->td->next.type != TOKEN_PAREN &&
          sd->tok.hash == LIT_ASYNC) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);
        context |= CONTEXT__ASYNC;
      }

      // ... look for '*'
      if (sd->tok.hash == MISC_STAR) {
        context |= CONTEXT__GENERATOR;
        record_walk(sd, -1);
      }

      // ... look for get/set without '(' next
      if (sd->td->next.type != TOKEN_PAREN &&
          (sd->tok.hash == LIT_GET || sd->tok.hash == LIT_SET)) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);
      }

      // terminal state of left side
      switch (sd->tok.type) {
        // ... anything that looks like it could be a function, that way (and let stack fail)
        case TOKEN_STRING:
          if (sd->tok.p[0] == '`' || sd->next->type != TOKEN_PAREN) {
            break;  // don't allow anything but " 'foo' ( "
          }
          // fall-through

        case TOKEN_LIT:
        case TOKEN_PAREN:
        case TOKEN_BRACE:
        case TOKEN_ARRAY:
          debugf("pretending to be function: %.*s\n", sd->tok.len, sd->tok.p);
          stack_inc(sd, SSTACK__FUNC);
          sd->curr->context = context;
          return 0;

        case TOKEN_COLON:
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__EXPR);
          debugf("pushing expr for colon\n");
          return 0;

        case TOKEN_CLOSE:
          --sd->curr;
          debugf("closing dict, value=%d level=%ld\n", sd->curr->stype == SSTACK__EXPR, sd->curr - sd->stack);
          skip_walk(sd, sd->curr->stype == SSTACK__EXPR);
          return 0;

        case TOKEN_OP:
          if (sd->tok.hash == MISC_COMMA) {
            return record_walk(sd, -1);
          }
          break;
      }

      // if this a single literal, it's valid: e.g. {'abc':def}
      // ... but we pretend it's an expression anyway (and : closes it)
      debugf("starting expr inside left dict\n");
      stack_inc(sd, SSTACK__EXPR);
      return 0;
    }

    // function state, allow () or {}
    case SSTACK__FUNC:
      switch (sd->tok.type) {
        case TOKEN_ARRAY:
          // allow "function ['name']" (for dict)
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__EXPR);
          sd->curr->start = TOKEN_ARRAY;

          // ... but "{async [await 'name']..." doesn't take await from our context
          sd->curr->context = (sd->curr - 2)->context;
          return 0;

        case TOKEN_STRING:
          // allow "function 'foo'" (for dict)
          if (sd->tok.p[0] == '`') {
            break;  // don't allow template literals
          }
          return record_walk(sd, 0);

        case TOKEN_LIT: {
          sstack *p = (sd->curr - 1);  // use context from parent, "async function await() {}" is valid :(

          // we're only maybe a keyword in non-dict modes
          if (p->stype != SSTACK__DICT && !is_valid_name(sd->tok.hash, p->context)) {
            sd->tok.type = TOKEN_KEYWORD;
          } else {
            sd->tok.type = TOKEN_SYMBOL;
          }
          return record_walk(sd, 0);
        }

        case TOKEN_PAREN:
          // allow "function ()"
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__EXPR);
          sd->curr->start = TOKEN_PAREN;
          return 0;

        case TOKEN_BRACE: {
          // terminal state of func, pop and insert normal block w/retained context
          uint8_t context = sd->curr->context;
          --sd->curr;
          sd->tok.type = TOKEN_EXEC;
          record_walk(sd, -1);
          stack_inc(sd, SSTACK__BLOCK);
          sd->curr->prev.type = TOKEN_TOP;
          sd->curr->context = context;
          return 0;
        }
      }

      // invalid, abandon function def
      debugf("invalid function construct\n");
      --sd->curr;
      return 0;

    // class state, just insert group (for extends) or dict-like
    case SSTACK__CLASS: {
      if (!sd->curr->prev.type && sd->tok.hash == LIT_EXTENDS) {
        // ... check for extends, valid
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);  // consume "extends" keyword, treat as non-value
        stack_inc(sd, SSTACK__EXPR);
        sd->curr->start = LIT_EXTENDS;
        return 0;
      }

      if (sd->tok.type == TOKEN_BRACE) {
        // start dict-like block (pop SSTACK__CLASS)
        --sd->curr;
        sd->tok.type = TOKEN_DICT;
        record_walk(sd, -1);
        stack_inc(sd, SSTACK__DICT);
        return 0;
      }

      // invalid, abandon class def
      debugf("invalid class construct\n");
      --sd->curr;
      return 0;
    }

    // control group state
    case SSTACK__CONTROL:
      --sd->curr;
      yield_virt(sd, TOKEN_ATTACH);
      // FIXME FIXME "do-while" case
      break;

    case SSTACK__EXPR:
      return simple_consume_expr(sd);

    default:
      debugf("unhandled stype=%d\n", sd->curr->stype);
      return ERROR__INTERNAL;

    // zero state, determine what to push
    case SSTACK__BLOCK:
      break;
  }

  // TODO: yield start token
  if (sd->tok.type != TOKEN_CLOSE) {
    if (sd->curr->prev.type != TOKEN_ATTACH) {
      yield_virt(sd, TOKEN_START);
    }
  }

  switch (sd->tok.type) {
    case TOKEN_BRACE:
      // anon block
      if (sd->curr->prev.type != TOKEN_ATTACH) {
        debugf("unattached exec block\n");
      }
      sd->tok.type = TOKEN_EXEC;
      record_walk(sd, -1);
      stack_inc(sd, SSTACK__BLOCK);
      return 0;

    case TOKEN_CLOSE:
      if (sd->curr == sd->stack) {
        // ... top-level, invalid CLOSE
        debugf("invalid close\n");
      } else {
        if (sd->curr->prev.type == TOKEN_ATTACH) {
          debugf("got CLOSE after ATTACH\n");
        }
        --sd->curr;  // pop out of block
      }

      // "function {}" that ends in statement/group has value
      skip_walk(sd, sd->curr->stype == SSTACK__EXPR);
      return 0;

    case TOKEN_LIT:
      break;  // only care about lit below

    case TOKEN_STRING:
      // match 'use strict'
      do {
        if (sd->curr->prev.type != TOKEN_TOP) {
          break;
        }

        // FIXME: gross, lookahead to confirm that 'use strict' is on its own or generate ASI
        if (sd->next->type == TOKEN_SEMICOLON) {
          // great
        } else {
          if (sd->next->line_no == sd->tok.line_no) {
            // ... can't generate ASI
            break;
          }
          if (sd->next->hash & _MASK_REL_OP) {
            // binary oplike cases ('in', 'instanceof')
            break;
          }
          if (sd->next->type == TOKEN_OP) {
            if (sd->next->hash != MISC_INCDEC) {
              // ... ++/-- causes ASI
              break;
            }
          } else if (!is_token_valuelike(sd->next)) {
            break;
          }
        }

        if (is_use_strict(&(sd->tok))) {
          debugf("setting 'use strict'\n");
          sd->curr->context |= CONTEXT__STRICT;
        }
      } while (0);

    default:
      goto block_bail;  // non-lit starts statement
  }

  // match label
  if (is_label(&(sd->tok), sd->curr->context) && sd->next->type == TOKEN_COLON) {
    sd->tok.type = TOKEN_LABEL;
    skip_walk(sd, -1);  // consume label
    skip_walk(sd, -1);  // consume colon
    yield_virt(sd, TOKEN_ATTACH);
    return 0;
  }

  // match label keyword (e.g. "break foo;")
  if (match_label_keyword(sd) >= 0) {
    return 0;
  }

  uint32_t outer_hash = sd->tok.hash;

  // match single-only
  if (outer_hash == LIT_DEBUGGER) {
    sd->tok.type = TOKEN_KEYWORD;
    record_walk(sd, 0);
    yield_restrict_asi(sd);
    return 0;
  }

  // match restricted statement starters
  if (outer_hash == LIT_RETURN || outer_hash == LIT_THROW) {
    sd->tok.type = TOKEN_KEYWORD;
    record_walk(sd, 0);

    // throw doesn't cause ASI, because it's invalid either way
    if (outer_hash == LIT_RETURN && yield_restrict_asi(sd)) {
      return 0;
    }

    stack_inc(sd, SSTACK__EXPR);
    sd->curr->start = LIT_RETURN;
    return 0;
  }

  // module valid cases at top-level
  if (sd->curr == sd->stack && sd->is_module) {

    // match "import" which starts a sstack special
    if (outer_hash == LIT_IMPORT) {
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);
      stack_inc(sd, SSTACK__MODULE);
      sd->curr->start = LIT_IMPORT;
      return 0;
    }

    // match "export" which is sort of a no-op, resets to default state
    if (outer_hash == LIT_EXPORT) {
      sd->tok.type = TOKEN_KEYWORD;
      record_walk(sd, 0);

      if (sd->tok.hash == MISC_STAR || sd->tok.type == TOKEN_BRACE) {
        stack_inc(sd, SSTACK__MODULE);
        sd->curr->start = LIT_EXPORT;
        return 0;
      }

      if (sd->tok.hash == LIT_DEFAULT) {
        sd->tok.type = TOKEN_KEYWORD;
        record_walk(sd, 0);
      }

      // interestingly: "export default function() {}" is valid and a decl
      // so classic JS rules around decl must have names are ignored
      // ... "export default if (..)" is invalid, so we don't care if you do wrong things after
      return 0;
    }

  }

  // match "var", "let" and "const"
  if (match_decl(sd) >= 0) {
    stack_inc(sd, SSTACK__EXPR);
    sd->curr->start = outer_hash;  // var, let or const
    return 0;
  }

  // match e.g., "if", "catch"
  if (outer_hash & _MASK_CONTROL) {
    sd->tok.type = TOKEN_KEYWORD;
    record_walk(sd, 0);

    // match "for await"
    if (outer_hash == LIT_FOR && sd->tok.hash == LIT_AWAIT) {
      // even outside strict/async mode, this is valid, but an error
      sd->tok.type = TOKEN_KEYWORD;
      skip_walk(sd, 0);
    }

    // no paren needed or found, request attach immediately
    if (!(outer_hash & _MASK_CONTROL_PAREN) || sd->tok.type != TOKEN_PAREN) {
      yield_virt(sd, TOKEN_ATTACH);
      return 0;
    }

    // if we need a paren, consume and create expr group
    stack_inc(sd, SSTACK__CONTROL);
    sd->curr->start = outer_hash;
    record_walk(sd, -1);  // record inside SSTACK__CONTROL
    stack_inc(sd, SSTACK__EXPR);
    sd->curr->start = TOKEN_PAREN;
    return 0;
  }

  // hoisted function or class
  if (enact_defn(sd)) {
    return 0;
  }

block_bail:
  // start a regular statement
  stack_inc(sd, SSTACK__EXPR);
  return 0;
}


int prsr_simple(tokendef *td, int is_module, prsr_callback cb, void *arg) {
#ifdef DEBUG
  char *start = td->buf;
#endif
  simpledef sd;
  bzero(&sd, sizeof(simpledef));
  sd.curr = sd.stack;
  sd.is_module = is_module;
  if (is_module) {
    sd.curr->context = CONTEXT__STRICT;
  }
  sd.td = td;
  sd.next = &(td->next);
  sd.cb = cb;
  sd.arg = arg;

  sd.curr->stype = SSTACK__BLOCK;
  record_walk(&sd, -1);
  sd.curr->prev.type = TOKEN_TOP;

  int unchanged = 0;
  int ret = 0;
  while (sd.tok.type) {
    char *prev = sd.tok.p;
    ret = simple_consume(&sd);
    if (ret) {
      break;
    }

    // check stack range
    int depth = sd.curr - sd.stack;
    if (depth >= __STACK_SIZE - 1 || depth < 0) {
      debugf("stack exception, depth=%d\n", depth);
      ret = ERROR__STACK;
      break;
    }

    // allow unchanged ptr for some attempts for state machine
    if (prev == sd.tok.p) {
      if (unchanged++ < 2) {
        continue;  // allow two runs
      }
      debugf("simple_consume didn't consume: %d %.*s\n", sd.tok.type, sd.tok.len, sd.tok.p);
      ret = ERROR__INTERNAL;
      break;
    }

    // success
    prev = sd.tok.p;
    unchanged = 0;
  }

  if (ret) {
    return ret;
  }

  int depth = (sd.curr - sd.stack);
  while (depth) {
    debugf("end: sending TOKEN_EOF at depth=%d\n", depth);
    simple_consume(&sd);

    int update = (sd.curr - sd.stack);
    if (update >= depth) {
      break;  // only allow state pop
    }
    depth = update;
  }
  skip_walk(&sd, -1);  // emit 'real' EOF

  if (sd.curr != sd.stack) {
#ifdef DEBUG
    debugf("err: stack is %ld too high\n", sd.curr - sd.stack);
    sstack *t = sd.stack;
    do {
      debugf("...[%ld] stype=%d\n", t - sd.stack, t->stype);
      cb(sd.arg, &(t->prev));
    } while (t != sd.curr && ++t);
#endif
    return ERROR__STACK;
  }
  return 0;
}
