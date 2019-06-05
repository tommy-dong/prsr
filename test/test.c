
#include "../token.h"
#include "../parser.h"
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>

typedef struct _testdef {
  const char *name;
  const char *input;
  int *expected;  // zero-terminated token types
  int is_module;
  struct testdef *next;  // for failures
} testdef;

typedef struct {
  testdef *def;
  int at;
  int len;
  int error;
} testactive;

static void testdef_step(void *arg, token *t) {
  testactive *active = (testactive *) arg;

  int actual = t->type;
  int expected = -1;

  ++active->at;
  if (active->at < active->len) {
    expected = active->def->expected[active->at];
  } else if (active->at == active->len) {
    expected = 0;
  }

  if (actual != expected) {
    printf("%d: actual=%d expected=%d `%.*s`\n", active->at, actual, expected, t->len, t->p);
    active->error = 1;
  } else {
    printf("%d: ok=%d `%.*s`\n", active->at, actual, t->len, t->p);
  }
}

int run_testdef(testdef *def) {
  tokendef td = prsr_init_token((char *) def->input);
  testactive active = {
    .def = def,
    .at = -1,
    .len = 0,
    .error = 0,
  };

  // count expected size
  while (def->expected[active.len]) {
    ++active.len;
  }

  printf(">> %s\n", def->name);

  int out = prsr_simple(&td, def->is_module, testdef_step, &active);
  while (active.at + 1 < active.len) {
    token fake;
    bzero(&fake, sizeof(token));
    fake.type = 0;
    testdef_step(&active, &fake);
  }

  if (out) {
    printf("ERROR: internal error (%d)\n", out);
    return out;
  } else if (active.at != active.len) {
    printf("ERROR: mismatched length, actual=%d expected=%d\n", active.at, active.len);
    return 1;
  } else if (active.error) {
    printf("ERROR\n");
    return active.error;
  }

  printf("OK!\n");
  return 0;
}

// defines a test for prsr: args must have a trailing comma
#define _test(_name, _input, ...) \
{ \
  testdef td; \
  td.name = _name; \
  td.input = _input; \
  td.is_module = _name[0] == '^'; \
  td.next = NULL; \
  int v[] = {__VA_ARGS__ TOKEN_EOF}; \
  td.expected = v; \
  int lerr = run_testdef(&td); \
  if (lerr) { \
    err |= lerr; \
    last->next = malloc(sizeof(testdef)); \
    last = (testdef *) last->next; \
    *last = td; \
    ++ecount; \
  } \
  printf("\n"); \
  ++count; \
}

int main() {
  int err = 0;
  int count = 0;
  int ecount = 0;
  testdef fail, *last = &fail;
  bzero(&fail, sizeof(fail));

  _test("zero", "\n");

  _test("single symbol", "foo",
    TOKEN_START,
    TOKEN_SYMBOL,    // foo
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("simple", "var x = 1;",
    TOKEN_START,
    TOKEN_KEYWORD,   // var
    TOKEN_SYMBOL,    // x
    TOKEN_OP,        // =
    TOKEN_NUMBER,    // 1
    TOKEN_SEMICOLON, // ;
  );

  _test("ternary", "a ? : :\n?:",
    TOKEN_START,
    TOKEN_SYMBOL,    // a
    TOKEN_TERNARY,   // ?
    TOKEN_CLOSE,     // :
    TOKEN_COLON,     // :
    TOKEN_START,
    TOKEN_TERNARY,   // ?
    TOKEN_CLOSE,     // :
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("let as symbol", "+let",
    TOKEN_START,
    TOKEN_OP,        // +
    TOKEN_SYMBOL,    // let
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("invalid keyword use ignored", "x = if (a) /123/",
    TOKEN_START,
    TOKEN_SYMBOL,    // x
    TOKEN_OP,        // =
    TOKEN_KEYWORD,   // if
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // a
    TOKEN_CLOSE,     // )
    TOKEN_OP,        // /
    TOKEN_NUMBER,    // 123
    TOKEN_OP,        // /
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("control keyword starts new statement on newline", "x =\n if (a) /123/",
    TOKEN_START,
    TOKEN_SYMBOL,    // x
    TOKEN_OP,        // =
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_KEYWORD,   // if
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // a
    TOKEN_CLOSE,     // )
    TOKEN_ATTACH,
    TOKEN_REGEXP,    // /123/
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("function decl regexp", "function foo(y) {} / 100 /",
    TOKEN_START,
    TOKEN_KEYWORD,   // function
    TOKEN_SYMBOL,    // foo
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // y
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_START,
    TOKEN_REGEXP,    // / 100 /
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("class decl regexp", "class {} / 100 /",
    TOKEN_START,
    TOKEN_KEYWORD,   // class
    TOKEN_DICT,      // {
    TOKEN_CLOSE,     // }
    TOKEN_START,
    TOKEN_REGEXP,    // / 100 /
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("function statement", "(function(y) {} / 100 /)",
    TOKEN_START,
    TOKEN_PAREN,     // (
    TOKEN_KEYWORD,   // function
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // y
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_OP,        // /
    TOKEN_NUMBER,    // 100
    TOKEN_OP,        // /
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("simple async arrow function", "async () => await /123/",
    TOKEN_START,
    TOKEN_LIT,       // async
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_KEYWORD,   // MARK async
    TOKEN_ARROW,     // =>
    TOKEN_OP,        // await
    TOKEN_REGEXP,    // /123/
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("async arrow function", "() => async () => await\n/123/\nawait /1/",
    TOKEN_START,
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_ARROW,     // =>
    TOKEN_LIT,       // async
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_KEYWORD,   // MARK async
    TOKEN_ARROW,     // =>
    TOKEN_OP,        // await
    TOKEN_REGEXP,    // /123/
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_SYMBOL,    // await
    TOKEN_OP,        // /
    TOKEN_NUMBER,    // 1
    TOKEN_OP,        // /
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("class statement", "x = class Foo extends {} { if(x) {} } /123/",
    TOKEN_START,
    TOKEN_SYMBOL,    // x
    TOKEN_OP,        // =
    TOKEN_KEYWORD,   // class
    TOKEN_SYMBOL,    // Foo
    TOKEN_KEYWORD,   // extends
    TOKEN_DICT,      // {
    TOKEN_CLOSE,     // }
    TOKEN_DICT,      // {
    TOKEN_SYMBOL,    // if
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // x
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
    TOKEN_OP,        // /
    TOKEN_NUMBER,    // 123
    TOKEN_OP,        // /
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("dict string name", "+{'abc'() {}}",
    TOKEN_START,
    TOKEN_OP,        // +
    TOKEN_DICT,      // {
    TOKEN_STRING,    // 'abc'
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("dict after comma", ",{}",
    TOKEN_START,
    TOKEN_OP,        // ,
    TOKEN_DICT,      // {
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("ASI for PostfixExpression", "a\n++\nb",
    TOKEN_START,
    TOKEN_SYMBOL,    // a
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_OP,        // ++
    TOKEN_SYMBOL,    // b
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("yield is symbol", "yield",
    TOKEN_START,
    TOKEN_SYMBOL,    // yield
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("dict keyword-ness", "void {async * get get() {}}",
    TOKEN_START,
    TOKEN_OP,        // void
    TOKEN_DICT,      // {
    TOKEN_KEYWORD,   // async
    TOKEN_OP,        // *
    TOKEN_KEYWORD,   // get
    TOKEN_SYMBOL,    // get
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("yield is op", "function*() { yield /123/ }",
    TOKEN_START,
    TOKEN_KEYWORD,   // function
    TOKEN_OP,        // *
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_OP,        // yield
    TOKEN_REGEXP,    // /123/
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("yield is restricted", "function*() { yield\n/123/ }",
    TOKEN_START,
    TOKEN_KEYWORD,   // function
    TOKEN_OP,        // *
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_OP,        // yield
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_REGEXP,    // /123/
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("ASI rule for yield is ignored in group", "function*() { (yield\n/123/) }",
    TOKEN_START,
    TOKEN_KEYWORD,   // function
    TOKEN_OP,        // *
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_PAREN,     // (
    TOKEN_OP,        // yield
    TOKEN_REGEXP,    // /123/
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("for() matches let keyword", "for(let x;let;);",
    TOKEN_START,
    TOKEN_KEYWORD,   // for
    TOKEN_PAREN,     // (
    TOKEN_KEYWORD,   // let
    TOKEN_SYMBOL,    // x
    TOKEN_SEMICOLON, // ;
    TOKEN_SYMBOL,    // let
    TOKEN_SEMICOLON, // ;
    TOKEN_CLOSE,     // )
    TOKEN_ATTACH,
    TOKEN_SEMICOLON, // ;
  );

  _test("for await() matches keyword", "for await(let x);",
    TOKEN_START,
    TOKEN_KEYWORD,   // for
    TOKEN_KEYWORD,   // await
    TOKEN_PAREN,     // (
    TOKEN_KEYWORD,   // let
    TOKEN_SYMBOL,    // x
    TOKEN_CLOSE,     // )
    TOKEN_ATTACH,
    TOKEN_SEMICOLON, // ;
  );

  _test("for(blah of foo) matches keyword", "for(const x of bar);",
    TOKEN_START,
    TOKEN_KEYWORD,   // for
    TOKEN_PAREN,     // (
    TOKEN_KEYWORD,   // const
    TOKEN_SYMBOL,    // x
    TOKEN_OP,        // of
    TOKEN_SYMBOL,    // bar
    TOKEN_CLOSE,     // )
    TOKEN_ATTACH,
    TOKEN_SEMICOLON, // ;
  );

  _test("strict mode", "'use strict'; protected + x;",
    TOKEN_START,
    TOKEN_STRING,    // 'use strict';
    TOKEN_SEMICOLON, // ;
    TOKEN_START,
    TOKEN_KEYWORD,   // protected
    TOKEN_OP,        // +
    TOKEN_SYMBOL,    // x
    TOKEN_SEMICOLON, // ;
  );

  _test("strict mode not after label", "foo: 'use strict'; protected;",
    TOKEN_START,
    TOKEN_LABEL,     // foo
    TOKEN_COLON,     // :
    TOKEN_ATTACH,
    TOKEN_STRING,    // 'use strict';
    TOKEN_SEMICOLON, // ;
    TOKEN_START,
    TOKEN_SYMBOL,    // protected
    TOKEN_SEMICOLON, // ;
  );

  _test("strict mode not in control", "if {'use strict';protected+x}",
    TOKEN_START,
    TOKEN_KEYWORD,   // if
    TOKEN_ATTACH,
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_STRING,    // 'use strict';
    TOKEN_SEMICOLON, // ;
    TOKEN_START,
    TOKEN_SYMBOL,    // protected
    TOKEN_OP,        // +
    TOKEN_SYMBOL,    // x
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("asi for number", "123\n'zing'",
    TOKEN_START,
    TOKEN_NUMBER,    // 123
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_STRING,    // 'zing'
    TOKEN_SEMICOLON, // ASI ;
  );

  // nb. last semi is needed, attached to "while(0)" on its own
  _test("do-while while sanity check", "do while(2) x\nwhile(1) while(0);",
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_NUMBER,    // 2
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // virt
    TOKEN_SYMBOL,    // x
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // virt
    TOKEN_CLOSE,     // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_NUMBER,    // 1
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_NUMBER,    // 0
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // virt
    TOKEN_SEMICOLON, // ;
    TOKEN_CLOSE,     // virt
  );

  _test("do-while inside control", "if do ; while(0) bar",
    TOKEN_KEYWORD,   // if
    TOKEN_EXEC,      // virt
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // virt
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_NUMBER,    // 0
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // virt
    TOKEN_SYMBOL,    // bar
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("do-while block", "do {} while ();",
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ;
  );

  _test("do-while ASIs", "do foo\nwhile(0)",
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // virt
    TOKEN_SYMBOL,    // foo
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_NUMBER,    // 0
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("do-while stack", "do;while()bar",
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // virt
    TOKEN_SEMICOLON, // ;
    TOKEN_CLOSE,     // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_SYMBOL,    // bar
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("do-while value-like", "do;while()\n/foo/",
    TOKEN_KEYWORD,   // do
    TOKEN_EXEC,      // virt
    TOKEN_SEMICOLON, // ;
    TOKEN_CLOSE,     // virt
    TOKEN_KEYWORD,   // while
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_REGEXP,    // /foo/
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("strict", "'use strict'; let",
    TOKEN_STRING,    // 'blah'
    TOKEN_SEMICOLON, // ;
    TOKEN_KEYWORD,   // let
  );

  _test("arrow ASI bug", "{_ => {}}",
    TOKEN_START,
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_SYMBOL,    // _
    TOKEN_ARROW,     // =>
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("arrow value bug", "{_ => {}/123/g;}",
    TOKEN_START,
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_SYMBOL,    // _
    TOKEN_ARROW,     // =>
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_OP,        // /
    TOKEN_NUMBER,    // 123
    TOKEN_OP,        // /
    TOKEN_SYMBOL,    // g
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("ASI in case", "switch { default: }",
    TOKEN_START,
    TOKEN_KEYWORD,   // switch
    TOKEN_ATTACH,
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_KEYWORD,   // default
    TOKEN_COLON,     // :
    TOKEN_CLOSE,     // }
  );

  _test("dict method after colon", "void {:,get x() {}}",
    TOKEN_START,
    TOKEN_OP,        // void
    TOKEN_DICT,      // {
    TOKEN_COLON,     // :
    TOKEN_OP,        // ,
    TOKEN_KEYWORD,   // get
    TOKEN_SYMBOL,    // x
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("dict closed on right", "+{x:}",
    TOKEN_START,
    TOKEN_OP,        // +
    TOKEN_DICT,      // {
    TOKEN_SYMBOL,    // x
    TOKEN_COLON,     // :
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("dict method", "void {[] () {}}",
    TOKEN_START,
    TOKEN_OP,        // void
    TOKEN_DICT,      // {
    TOKEN_ARRAY,     // [
    TOKEN_CLOSE,     // ]
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("^check import", "import foo, {zing as what} from 'blah'",
    TOKEN_START,
    TOKEN_KEYWORD,   // import
    TOKEN_SYMBOL,    // foo
    TOKEN_OP,        // ,
    TOKEN_DICT,      // {
    TOKEN_SYMBOL,    // zing
    TOKEN_KEYWORD,   // as
    TOKEN_SYMBOL,    // what
    TOKEN_CLOSE,     // }
    TOKEN_KEYWORD,   // from
    TOKEN_STRING,    // 'blah'
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("static", "class X { static x() {} }",
    TOKEN_START,
    TOKEN_KEYWORD,   // class
    TOKEN_SYMBOL,    // X
    TOKEN_DICT,      // {
    TOKEN_KEYWORD,   // static
    TOKEN_SYMBOL,    // x
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
  );

  _test("async assumed with dot", ".async()",
    TOKEN_START,
    TOKEN_OP,        // .
    TOKEN_SYMBOL,    // async
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("using await as name inside async", "async () => class await {}",
    TOKEN_START,
    TOKEN_LIT,       // async
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_KEYWORD,   // MARK async
    TOKEN_ARROW,     // =>
    TOKEN_KEYWORD,   // class
    TOKEN_KEYWORD,   // await
    TOKEN_DICT,      // {
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("escape string", "'foo\\'bar'",
    TOKEN_START,
    TOKEN_STRING,    // 'foo\'bar'
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("doesn't consume label", "{break}",
    TOKEN_START,
    TOKEN_EXEC,      // {
    TOKEN_START,
    TOKEN_KEYWORD,   // break
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_CLOSE,     // }
  );

  _test("hoist function stops statement", "abc\nfunction foo() {}",
    TOKEN_START,
    TOKEN_SYMBOL,    // abc
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_KEYWORD,   // function
    TOKEN_SYMBOL,    // foo
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
  );

  _test("async part of function", "async\nfunction\nfoo() {}",
    TOKEN_START,
    TOKEN_KEYWORD,   // async
    TOKEN_KEYWORD,   // function
    TOKEN_SYMBOL,    // foo
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
  );

  _test("await should treat ~ as unary op", "await ~123",
    TOKEN_START,
    TOKEN_KEYWORD,   // await
    TOKEN_OP,        // ~
    TOKEN_NUMBER,    // 123
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("await should treat + as regular op", "await +123",
    TOKEN_START,
    TOKEN_SYMBOL,    // await
    TOKEN_OP,        // +
    TOKEN_NUMBER,    // 123
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("hashbang as comment", "#!hello",
    TOKEN_COMMENT,   // #!hello
  );

  _test("hashbang as comment with more", "#!hello /*\nfoo",
    TOKEN_COMMENT,   // #!hello /*
    TOKEN_START,
    TOKEN_SYMBOL,    // foo
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("hashbang with following comment", "#!hello\n//foo",
    TOKEN_COMMENT,   // #!hello
    TOKEN_COMMENT,   // //foo
  );

  _test("class extends op-like", "class X extends foo.bar { if() {} }",
    TOKEN_START,
    TOKEN_KEYWORD,   // class
    TOKEN_SYMBOL,    // X
    TOKEN_KEYWORD,   // extends
    TOKEN_SYMBOL,    // foo
    TOKEN_OP,        // .
    TOKEN_SYMBOL,    // bar
    TOKEN_DICT,      // {
    TOKEN_SYMBOL,    // if
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_CLOSE,     // }
  );

  _test("ternary has no value", "?:/foo/",
    TOKEN_START,
    TOKEN_TERNARY,   // ?
    TOKEN_CLOSE,     // :
    TOKEN_REGEXP,    // /foo/
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("control with trailing statement", "if foo\nbar",
    TOKEN_START,
    TOKEN_KEYWORD,   // if
    TOKEN_ATTACH,
    TOKEN_SYMBOL,    // foo
    TOKEN_SEMICOLON, // ASI ;
    TOKEN_START,
    TOKEN_SYMBOL,    // foo
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("attach statements", "if()try{}finally{}",
    TOKEN_START,
    TOKEN_KEYWORD,   // if
    TOKEN_PAREN,     // (
    TOKEN_CLOSE,     // )
    TOKEN_ATTACH,
    TOKEN_KEYWORD,   // try
    TOKEN_ATTACH,
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
    TOKEN_START,
    TOKEN_KEYWORD,   // finally
    TOKEN_ATTACH,
    TOKEN_EXEC,      // {
    TOKEN_CLOSE,     // }
  );

  _test("solo async", "async(a)",
    TOKEN_START,
    TOKEN_LIT,       // async
    TOKEN_PAREN,     // (
    TOKEN_SYMBOL,    // a
    TOKEN_CLOSE,     // )
    TOKEN_SYMBOL,    // MARK async
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("label inside block", "if foo: 1",
    TOKEN_START,
    TOKEN_KEYWORD,   // if
    TOKEN_ATTACH,
    TOKEN_LABEL,     // foo
    TOKEN_COLON,     // :
    TOKEN_ATTACH,
    TOKEN_NUMBER,    // 1
    TOKEN_SEMICOLON, // ASI ;
  );

  _test("return dict", "return {}",
    TOKEN_START,
    TOKEN_KEYWORD,   // return
    TOKEN_DICT,      // {
    TOKEN_CLOSE,     // }
    TOKEN_SEMICOLON, // ASI ;
  );

  // restate all errors
  testdef *p = &fail;
  if (ecount) {
    printf("errors (%d/%d):\n", ecount, count);
  }
  while ((p = (testdef *) p->next)) {
    printf("-- %s\n", p->name);
  }
  return err;
}
