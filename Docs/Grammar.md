# DreamAnim grammar and canonical format

This is the reference the lexer, the parser and the printer implement. It covers the syntax only;
what a name means (a node type, a C++ property, an asset slot) is decided by the later stages.

## Files

| Extension | Document kind | Produces |
|---|---|---|
| `.dab` | `AnimBlueprint(...)` | an Animation Blueprint |
| `.dai` | `AnimLayerInterface(...)` | an Animation Layer Interface |
| `.dat` | `AnimTemplates(...)` | nothing; `#include`d by other files |

Text is UTF-8 (a BOM is accepted). Line ends are `\n`, `\r\n` or a lone `\r`.

## Lexical structure

- Comments: `// …` to the end of the line, `/* … */` (not nested). Kept as trivia when asked.
- Identifiers: a letter (any Unicode letter counts, so Chinese is fine) or `_`, then letters, digits, `_`.
  Case-sensitive. There are no reserved words at the lexical level: `true`, `false`, `State`,
  `Settings` and the rest are identifiers that the parser recognises by position.
- Numbers: `12`, `0.5`, `.5`, `1e-3`, `2.5E+4`. No sign (a leading `-` is the unary operator), no suffix.
- Strings: `"…"` on one line. Escapes: `\"`, `\\`, `\n`, `\t`, `\r`. Any other escape is DAB1006.
- Directives: `#` at the start of a line followed by a word: `#include`, `#Region`, `#EndRegion`.
  Any other word is DAB1005.
- Punctuation and operators:
  `( ) { } [ ] , ; : . ? = -> == != < <= > >= + - * / ! && ||`

## Syntax (EBNF)

```
File        = { Include } Document { Include-misplaced } ;
Include     = "#include" String ;
Document    = DocKind "(" [ Arguments ] ")" Block ;
DocKind     = "AnimBlueprint" | "AnimLayerInterface" | "AnimTemplates" ;

Block       = "{" { Statement } "}" ;
Statement   = Assign | Decl | Transition | NamedBlock | Region ;
Assign      = Key "=" Expr [ Meta ] Terminator ;
Key         = Ident | String ;
Decl        = Ident Ident [ "=" Expr ] [ Meta ] ";" ;            (* type name, then name *)
Transition  = Ident "->" Ident ":" Expr [ Meta ] ";" ;
NamedBlock  = BlockWord Ident [ Params ] [ Meta ]
              ( Block [ ";" ] | "=" Expr ";" | ";" ) ;
BlockWord   = "StateMachine" | "State" | "Layer" | "Template" | "Conduit" | "Alias" ;
Params      = "(" [ Param { "," Param } ] ")" ;                   (* Template only *)
Param       = Ident [ "=" Expr ] ;
Region      = "#Region" String | "#EndRegion" ;
Terminator  = ";" | (* nothing, when Expr ends with "}" *) ;

Meta        = "[" [ MetaEntry { ( ";" | "," ) MetaEntry } [ ";" | "," ] ] "]" ;
MetaEntry   = Key [ "=" Expr ] ;                                 (* no value = a flag *)

Expr        = Ternary ;
Ternary     = Or [ "?" Expr ":" Expr ] ;
Or          = And { "||" And } ;
And         = Equality { "&&" Equality } ;
Equality    = Compare { ( "==" | "!=" ) Compare } ;
Compare     = Additive { ( "<" | "<=" | ">" | ">=" ) Additive } ;
Additive    = Multiply { ( "+" | "-" ) Multiply } ;
Multiply    = Unary { ( "*" | "/" ) Unary } ;
Unary       = ( "!" | "-" ) Unary | Postfix ;
Postfix     = Primary { "." Ident | "(" [ Arguments ] ")" } ;
Primary     = Number | String | "true" | "false" | Ident
            | "(" Expr ")"                                       (* grouping *)
            | "(" Arguments ")"                                  (* tuple: none, two or more items, or any named item; `()` is a struct at its defaults *)
            | "[" [ Expr { "," Expr } [ "," ] ] "]"              (* list *)
            | Block                                              (* map: statements *)
            | Stack ;
Stack       = "{" Expr ";" { Expr ";" } "}" ;
Arguments   = Argument { "," Argument } [ "," ] ;
Argument    = [ Key "=" ] Expr ;
```

### Telling a block from a stack

Both start with `{`. The parser looks at the first tokens after it:

| After `{` | It is |
|---|---|
| `}` | an empty block |
| `Ident =` or `String =` (`=` not followed by `=`) | a block |
| `Ident Ident` | a block (a declaration) |
| `Ident ->` | a block (a transition) |
| one of the block words followed by `Ident` | a block |
| `#Region` / `#EndRegion` | a block |
| anything else | a stack |

A block whose statements turn out to mix in a bare expression, or a stack that turns out to hold an
assignment, is DAB2010.

### Notes

- `true` and `false` parse as boolean literals wherever an expression is expected.
- A `[` right after a complete expression opens the statement's metadata; there is no index operator.
- Positional arguments come before named ones (DAB2012). A name used twice in one argument list is DAB2011.
- `#include` lines come before the document. One document per file (DAB2003).
- `#Region` / `#EndRegion` pair up inside one block; an unpaired one is a warning (DAB2008).

## Canonical format

The printer turns any parse into one canonical text. Formatting twice gives the same text as formatting
once, and the canonical text parses to the same tree.

- Indentation: four spaces. Line width: 110 columns.
- One statement per line. A run of blank lines between statements becomes one blank line; blank
  lines at the start and end of a block are removed.
- Comments stay where they were relative to their statement: own-line comments above it, a
  same-line comment after it, comments after the last statement of a block before its `}`.
- Document: `Kind(Name="…", Root="…")` on one line, `{` on the next line, statements, `}`.
- An assignment whose value is a block prints without `;`; every other assignment ends with `;`.
- Statement-level blocks and stacks are broken over lines: `X = {` / entries / `}`. The exception is
  a block of exactly one statement that is neither a named block nor a block or stack itself and has
  no comments: it prints on one line when it fits (`Aliases = { Main = Self.GetParent(); }`).
  Blocks and stacks inside an argument list print on one line when they fit, otherwise broken.
- Calls print on one line when they fit. Otherwise every argument goes on its own line, one level
  deeper, with the `=` of named arguments aligned, and the `)` on its own line at the call's indent:

  ```
  EmitterState(
      LifeCycleMode    = Self,
      LoopDuration     = 0.15
  );
  ```
- Lists and tuples print on one line when they fit, otherwise one item per line like arguments.
- Alignment inside a block, over each run of consecutive single-line statements of the same kind
  (a blank line, a comment line or a multi-line statement ends the run):
  - assignments: the `=` signs line up;
  - declarations: the type, name and `=` columns line up;
  - transitions: the `->`, the `:` and the metadata `[` line up.
- Metadata: `[ Key = Value; Flag ]` on one line, entries separated by `; `.
- Numbers print as written. Strings print with the escapes above re-applied.
- Named blocks: `State Idle` then `{` on the next line at the same indent; a body that qualifies for
  the one-line rule above stays on the head's line (`State Stop { Pose = StopStates; }`); a named
  block with a value prints on one line `Layer Overlay = LocalRefPose();`.
- Parentheses the author wrote are kept.

## Diagnostics raised by the front end

| Code | Severity | Meaning |
|---|---|---|
| DAB1001 | Error | A character that cannot start any token |
| DAB1002 | Error | A string not closed on its line |
| DAB1003 | Error | A block comment not closed |
| DAB1004 | Error | A malformed number |
| DAB1005 | Error | An unknown directive |
| DAB1006 | Error | An unknown escape in a string |
| DAB2001 | Error | Expected one thing, found another |
| DAB2002 | Error | No document in the file |
| DAB2003 | Error | More than one document in the file |
| DAB2004 | Error | Unknown document kind |
| DAB2005 | Error | The document header lacks `Name` or has an argument other than `Name` / `Root` |
| DAB2006 | Error | Expected an expression |
| DAB2007 | Error | A bracket, brace or parenthesis not closed before the end of the file |
| DAB2008 | Warning | `#Region` / `#EndRegion` not paired within the block |
| DAB2009 | Error | `#include` after the document |
| DAB2010 | Error | A block mixed with stack steps, or a stack with statements |
| DAB2011 | Error | The same argument name twice in one argument list |
| DAB2012 | Error | A positional argument after a named one |
| DAB2013 | Error | Malformed metadata |
| DAB2014 | Error | A template parameter declared twice, or a parameter without a default before one with |
| DAB3101 | Error | A template call misses an argument that has no default |
| DAB3102 | Error | A template call names a parameter the template does not have |
| DAB3103 | Error | A template call has more arguments than the template has parameters |
| DAB3104 | Error | Template expansion nested too deep (a template that calls itself) |
| DAB3105 | Error | Two templates with the same name (in the file and its includes) |
| DAB3106 | Error | A template written without `= expression;` (a block body or nothing) |
| DAB3201 | Error | An `#include`d file was not found |
| DAB3202 | Error | `#include` cycle |
| DAB3203 | Error | An `#include`d file is not an `AnimTemplates` library |
