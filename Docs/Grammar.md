# DreamAnim grammar and canonical format

This is the reference the lexer, the parser and the printer implement. It covers the syntax only;
what a name means (a node type, a C++ property, an asset slot) is decided by the later stages.

## Files

| Extension | Document kind | Produces |
|---|---|---|
| `.dab` | `AnimBlueprint(...)` | an Animation Blueprint |
| `.dai` | `AnimLayerInterface(...)` | an Animation Layer Interface |
| `.dat` | `AnimTemplates(...)` | nothing; `#include`d by other files |
| `.dcr` | `ControlRig(...)` | a Control Rig |

Text is UTF-8 (a BOM is accepted). Line ends are `\n`, `\r\n` or a lone `\r`.

## Lexical structure

- Comments: `// …` to the end of the line, `/* … */` (not nested). Kept as trivia when asked.
- Identifiers: a letter (any Unicode letter counts, so Chinese is fine) or `_`, then letters, digits, `_`.
  Case-sensitive. There are no reserved words at the lexical level: `true`, `false`, `State`,
  `Settings` and the rest are identifiers that the parser recognises by position.
- Names in backticks: `` `Module Settings` `` is one identifier whose text is what stands between the
  backticks, spaces and case kept. It can be written wherever an identifier can: statement keys,
  argument names, members, block names, parameters, types, callees. A name in backticks is never read
  as a word the parser recognises by position (`` `true` `` is a name, not a boolean). The name is not
  empty and has no line break or backtick in it (DAB1007).
- Numbers: `12`, `0.5`, `.5`, `1e-3`, `2.5E+4`. No sign (a leading `-` is the unary operator), no suffix.
- Strings: `"…"` on one line. Escapes: `\"`, `\\`, `\n`, `\t`, `\r`. Any other escape is DAB1006.
- Directives: `#` at the start of a line followed by a word: `#include`, `#Region`, `#EndRegion`.
  Any other word is DAB1005.
- Punctuation and operators:
  `( ) { } [ ] , ; : . ? = -> == != < <= > >= + - * / % ! && ||`

## Syntax (EBNF)

```
File        = { Include } Document { Include-misplaced } ;
Include     = "#include" String ;
Document    = DocKind "(" [ Arguments ] ")" Block ;
DocKind     = "AnimBlueprint" | "AnimLayerInterface" | "AnimTemplates" | "ControlRig" ;

Block       = "{" { Statement } "}" ;
Statement   = Assign | Decl | Transition | NamedBlock | Region ;
Assign      = Key "=" Expr [ Meta ] Terminator ;
Key         = Name | String ;
Name        = Ident | BacktickName ;
Decl        = Type Name [ "=" Expr ] [ Meta ] ";" ;               (* type name, then name *)
Type        = Name [ "<" Type { "," Type } ">" ] ;                (* the type arguments: ControlRig only *)
Transition  = Name "->" Name ":" Expr [ Meta ] ";" ;
NamedBlock  = BlockWord Name [ Params ] [ Meta ]
              ( Block [ ";" ] | "=" Expr ";" | ";" ) ;
BlockWord   = "StateMachine" | "State" | "Layer" | "Template" | "Conduit" | "Alias" ;
Params      = "(" [ Param { "," Param } ] ")" ;                   (* Template only *)
Param       = Name [ "=" Expr ] ;
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
Multiply    = Unary { ( "*" | "/" | "%" ) Unary } ;
Unary       = ( "!" | "-" ) Unary | Postfix ;
Postfix     = Primary { "." Name | "(" [ Arguments ] ")" | "[" Expr "]" } ;   (* the index: inside code only *)
Primary     = Number | String | "true" | "false" | Name
            | "(" Expr ")"                                       (* grouping *)
            | "(" Arguments ")"                                  (* tuple: none, two or more items, or any named item; `()` is a struct at its defaults *)
            | "[" [ Expr { "," Expr } [ "," ] ] "]"              (* list *)
            | Block                                              (* map: statements *)
            | Stack ;
Stack       = "{" Expr ";" { Expr ";" } "}" ;
Arguments   = Argument { "," Argument } [ "," ] ;
Argument    = [ Key "=" ] Expr ;
```

### Control Rig documents

A `ControlRig` document adds the following; nothing here is recognised in any other document.

```
BlockWord   = ... | "Event" | "Function" | "Detached"                (* the body is code *)
                  | "Bone" | "Null" | "Control" | "Curve" | "Connector" | "Socket" ;   (* the body is a Block *)
NamedBlock  = "Event" Name [ Meta ] CodeBlock
            | "Function" Name [ TypedParams ] [ "->" TypedParams ] [ Meta ] ( CodeBlock | "=" Expr ";" )
            | "Detached" [ Meta ] CodeBlock                       (* no name *)
            | ElementWord Name [ Meta ] ( Block [ ";" ] | ";" ) ;
ElementWord = "Bone" | "Null" | "Control" | "Curve" | "Connector" | "Socket" ;
TypedParams = "(" [ TypedParam { "," TypedParam } ] ")" ;          (* `()` is still a parameter list *)
TypedParam  = Type Name [ "=" Expr ] ;                            (* an output takes no default *)

CodeBlock   = "{" { CodeStmt } "}" ;
CodeStmt    = Let | Typed | Local | Set | ExprStmt | If | For | Return | DetachedStmt | Region ;
Let         = "let" Name "=" Expr ";" ;
Typed       = Type Name "=" Expr ";" ;                            (* a let with its type written *)
Local       = "local" Type Name [ "=" Expr ] ";" ;
Set         = Postfix "=" Expr ";" ;                              (* a name, a member or an element *)
ExprStmt    = Expr ";" ;
If          = "if" "(" Expr ")" CodeBlock { "else" "if" "(" Expr ")" CodeBlock } [ "else" CodeBlock ] ;
For         = "for" "(" Name { "," Name } "in" Expr ")" CodeBlock ;   (* one to four names *)
Return      = "return" [ "(" Argument { "," Argument } ")" ] ";" ;    (* every item named *)
DetachedStmt = "Detached" CodeBlock ;
```

- The words `let`, `local`, `if`, `else`, `for`, `in`, `return` and `Detached` are recognised by position
  only: at the start of a code statement (`in` inside the header of a `for`), and only when they are not
  written in backticks. Anywhere else they are names like any other, and in code a name that is one of
  them is written in backticks.
- A code statement starts with `Type Name` followed by `=` when it is a typed let. A type with arguments
  (`Array<RigElementKey>`, `TArray<double>`) is stored as written without spaces.
- Code statements take no metadata. Inside code a `[` after an expression is an index, so
  `Chain[2] = Bone(thigh_l);` is an assignment to an element. A `[` followed by `Key =`, `Key;` or
  `Key,` can only have been meant as metadata and is DAB2015; `[ Key ]` alone reads as an index.
- `else` needs an `if` before it (DAB2016). A `for` header names one to four values and `in` (DAB2017).
  A `return` gives its outputs as a tuple of named items (DAB2018).
- The value of a function written `= Expr;` is code in this respect: `[` after an expression is an index.
- `#Region` / `#EndRegion` pair up inside one code block like inside a block.

### Telling a block from a stack

Both start with `{`. The parser looks at the first tokens after it:

| After `{` | It is |
|---|---|
| `}` | an empty block |
| `Ident =` or `String =` (`=` not followed by `=`) | a block |
| `Ident Ident` | a block (a declaration) |
| `Ident < Ident > Ident` (`ControlRig` only; nested and comma-separated type arguments too) | a block (a declaration of a generic type) |
| `Ident ->` | a block (a transition) |
| one of the block words followed by `Ident` (in a `ControlRig`, its extra block words as well) | a block |
| `#Region` / `#EndRegion` | a block |
| anything else | a stack |

A block whose statements turn out to mix in a bare expression, or a stack that turns out to hold an
assignment, is DAB2010.

### Notes

- `true` and `false` parse as boolean literals wherever an expression is expected.
- A `[` right after a complete expression opens the statement's metadata. The index operator `a[i]` exists
  only inside code blocks, where statements take no metadata (see "Control Rig documents").
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
- Names: a name prints as written when it is a plain identifier (a letter or `_`, then letters, digits and
  `_`; any Unicode letter counts) and otherwise in backticks. Inside code a name that is a code word
  (`let`, `local`, `if`, `else`, `for`, `in`, `return`, `Detached`) prints in backticks too, and so does
  a name spelled `true` or `false` wherever a value is read. This holds for every place a name is
  printed: keys, argument names, members, declarations, block names, parameters, `for` names, callees.
- Types with arguments print as `Array<RigElementKey>`: no spaces, the arguments separated by `,`.

### Canonical format of code

- A body of code is Allman, like a named block: `Event Forward` (or `Function ...` or `Detached`), then `{`
  on its own line at the same indent, the statements one level deeper, `}` at the head's indent. An empty
  body is `{` and `}` on two lines.
- One statement per line, each ending with `;`. A run of blank lines between statements becomes one;
  blank lines at the start and end of a body are removed. Comments keep their place: own-line comments
  above the statement, a same-line comment after it, comments after the last statement before the `}`.
- `if (condition)`, the block, then `else if (condition)` and its block, then `else` and its block, each
  keyword on a line of its own at the indent of the `if`:

  ```
  if (Weight > 0.5)
  {
      ...
  }
  else if (Alpha > 0)
  {
      ...
  }
  else
  {
      ...
  }
  ```
- `for (Element, Index in Items)` then the block; `Detached` then the block; `return (Out = value);`.
- A function: `Function Name(Type a, Type b = 1) -> (Type c) [ meta ]` on one line (no arrow when there
  are no outputs), then the block; or the head, ` = Expr;` on the same line when it fits, otherwise
  broken like any value. An event: `Event Name [ meta ]`.
- Calls, lists and tuples wrap as above: one argument per line, the `=` of named ones aligned, the `)` on
  a line of its own. An index prints on the line of its object: `Chain[2]`.
- Alignment over a run of consecutive single-line statements of one kind (a blank line, a comment line or
  a multi-line statement ends the run): `let` statements (typed lets and locals with a value too) line
  their `=` up; assignments (`Target = value;`) line their `=` up. Declarations in a section align type,
  name and `=` columns as before, a type with arguments counting at its printed width.

## Diagnostics raised by the front end

| Code | Severity | Meaning |
|---|---|---|
| DAB1001 | Error | A character that cannot start any token |
| DAB1002 | Error | A string not closed on its line |
| DAB1003 | Error | A block comment not closed |
| DAB1004 | Error | A malformed number |
| DAB1005 | Error | An unknown directive |
| DAB1006 | Error | An unknown escape in a string |
| DAB1007 | Error | A name in backticks that is empty or not closed on its line |
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
| DAB2014 | Error | A template parameter declared twice, or a parameter without a default before one with; a function parameter or output declared twice |
| DAB2015 | Error | Metadata (`[ Key = Value ]`) after a code statement |
| DAB2016 | Error | `else` without an `if` before it |
| DAB2017 | Error | A malformed `for` header: no name, more than four names, or no `in` |
| DAB2018 | Error | A `return` value that is not a tuple of named items |
| DAB3101 | Error | A template call misses an argument that has no default |
| DAB3102 | Error | A template call names a parameter the template does not have |
| DAB3103 | Error | A template call has more arguments than the template has parameters |
| DAB3104 | Error | Template expansion nested too deep (a template that calls itself) |
| DAB3105 | Error | Two templates with the same name (in the file and its includes) |
| DAB3106 | Error | A template written without `= expression;` (a block body or nothing) |
| DAB3201 | Error | An `#include`d file was not found |
| DAB3202 | Error | `#include` cycle |
| DAB3203 | Error | An `#include`d file is not an `AnimTemplates` library |
