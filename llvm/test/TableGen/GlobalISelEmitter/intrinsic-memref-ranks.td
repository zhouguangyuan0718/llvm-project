// RUN: llvm-tblgen -gen-global-isel -optimize-match-table=false -I %p/../Common -I %p/../../../include %s | FileCheck -check-prefix=GISEL %s

include "llvm/Target/Target.td"
include "GlobalISelEmitterCommon.td"

// LLVM intrinsic signatures are statically typed, so rank is encoded in the
// descriptor type. Use the helper multiclass to define a family of concrete
// rank variants with one declaration.
let TargetPrefix = "mytarget" in {
  defm int_mytarget_memref_add : LLVMMemRefIntrinsic1To5<
      [], [], [], llvm_i32_ty, llvm_i32_ty>;
}

// Rank-2 descriptor flattens to 7 i32 operands:
//   allocated, aligned, offset, size0, size1, stride0, stride1.
// GISEL: GIM_CheckIntrinsicID, /*MI*/0, /*Op*/0, GIMT_Encode2(Intrinsic::mytarget_memref_add_rank2),
// GISEL: // (intrinsic_void {{[0-9]+}}:{ *:[iPTR] }, i32:$a, i32:$b, i32:$c, i32:$d, i32:$e, i32:$f, i32:$g)
def MEMREF_RANK2_USE : I<(outs),
  (ins GPR32:$a, GPR32:$b, GPR32:$c, GPR32:$d, GPR32:$e, GPR32:$f, GPR32:$g),
  [(int_mytarget_memref_add_rank2 GPR32:$a, GPR32:$b, GPR32:$c, GPR32:$d,
                              GPR32:$e, GPR32:$f, GPR32:$g)]>;

// Rank-3 descriptor flattens to 9 i32 operands:
//   allocated, aligned, offset, size0, size1, size2, stride0, stride1, stride2.
// GISEL: GIM_CheckIntrinsicID, /*MI*/0, /*Op*/0, GIMT_Encode2(Intrinsic::mytarget_memref_add_rank3),
// GISEL: // (intrinsic_void {{[0-9]+}}:{ *:[iPTR] }, i32:$a, i32:$b, i32:$c, i32:$d, i32:$e, i32:$f, i32:$g, i32:$h, i32:$i)
def MEMREF_RANK3_USE : I<(outs),
  (ins GPR32:$a, GPR32:$b, GPR32:$c, GPR32:$d, GPR32:$e,
       GPR32:$f, GPR32:$g, GPR32:$h, GPR32:$i),
  [(int_mytarget_memref_add_rank3 GPR32:$a, GPR32:$b, GPR32:$c, GPR32:$d,
                              GPR32:$e, GPR32:$f, GPR32:$g, GPR32:$h,
                              GPR32:$i)]>;
