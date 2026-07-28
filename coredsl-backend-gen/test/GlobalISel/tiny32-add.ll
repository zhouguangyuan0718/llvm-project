; Minimal LLVM IR accepted by the generated GlobalISel-only target. The
; function has two i32 register arguments and one i32 return, which are the
; intentionally narrow ABI subset implemented by the generated scaffolding.

define i32 @tiny32_arithmetic(i32 %lhs, i32 %rhs) nounwind optnone noinline {
entry:
  %sum = add i32 %lhs, %rhs
  %difference = sub i32 %sum, %rhs
  ret i32 %difference
}
