# Operation Canonicalization in MLIR

Canonicalization is an important part of compiler IR design - it makes it easier
to implement reliable compiler transformations, be able to reason about what is
better or worse in the code, and forces interesting discussions about what the
goals of a particular level of IR are interested in. Dan Gohman wrote
[an article](https://sunfishcode.github.io/blog/2018/10/22/Canonicalization.html)
exploring these issues, it is worth reading if you're not familiar with these
concepts.

Most compilers have canonicalization passes, and sometimes they have many
different ones (e.g. instcombine, dag combine, etc in LLVM). Because MLIR is a
multi-level IR, we can provide a single canonicalization infrastructure and
reuse it across many different IRs that it represents. This document describes
the general approach, global canonicalizations performed, and provides sections
to capture IR specific rules for reference.

## General Design

MLIR has a single canonicalization pass, which iteratively applies
canonicalization transformations in a greedy way until the IR converges. These
transformations are defined by the operations themselves, which allows each
dialect to define its own set of operations and canonicalizations together.

Some important things to think about w.r.t. canonicalization patterns:

*   Repeated applications of patterns should converge. Unstable or cyclic
    rewrites will cause infinite loops in the canonicalizer.

*   It is generally better to canonicalize towards operations that have fewer
    uses of a value when the operands are duplicated, because some patterns only
    match when a value has a single user. For example, it is generally good to
    canonicalize "x + x" into "x * 2", because this reduces the number of uses
    of x by one.

TODO: These patterns are currently defined directly in the canonicalization
pass, but they will be split out soon.

## Globally Applied Rules

These transformation are applied to all levels of IR:

*   Constant folding - e.g. "(addi 1, 2)" to "3". Constand folding hooks are
    specified by operations.

*   Move constant operands to commutative binary operators to the right side -
    e.g. "(addi 4, x)" to "(addi x, 4)".

## Builtin Ops Canonicalizations

These transformations are applied to builtin ops:

*   `constant` ops are uniqued and hoisted into the entry block of a function.
*   (TODO) Merge `affine_apply` instructions that directly feed each other.

## Standard Ops Canonicalizations

*   Shape folding of `alloc` instructions to turn dynamic dimensions into static
    ones.
*   Folding `memref_cast` instructions into users where possible.