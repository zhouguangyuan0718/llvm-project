// RUN: llvm-tblgen -gen-global-isel -optimize-match-table=false -I %p/../Common -I %p/../../../include %s | FileCheck -check-prefix=GISEL %s

include "llvm/Target/Target.td"
include "GlobalISelEmitterCommon.td"

def GPR : RegisterClass<"MyTarget", [i32, i64, p0], 32, (add R0)>;

// LLVM intrinsic signatures are statically typed, so rank is encoded in the
// descriptor type. Use the helper multiclass to define a family of concrete
// rank variants with one declaration.
let TargetPrefix = "mytarget" in {
  defm int_mytarget_memref_add : LLVMMemRefIntrinsic1To5<
      [], [llvm_memref_ty]>;
}

// Rank-2 descriptor flattens to 7 operands (ptr, ptr, i64, i64, i64, i64, i64).
// GISEL: GIM_CheckIntrinsicID, /*MI*/0, /*Op*/0, GIMT_Encode2(Intrinsic::mytarget_memref_add_rank2),
def MEMREF_RANK2_USE : I<(outs),
  (ins GPR:$a, GPR:$b, GPR:$c, GPR:$d, GPR:$e, GPR:$f, GPR:$g),
  [(int_mytarget_memref_add_rank2 GPR:$a, GPR:$b, GPR:$c, GPR:$d,
                              GPR:$e, GPR:$f, GPR:$g)]>;

// Rank-3 descriptor flattens to 9 operands (ptr, ptr, i64, i64, i64, i64, i64, i64, i64).
// GISEL: GIM_CheckIntrinsicID, /*MI*/0, /*Op*/0, GIMT_Encode2(Intrinsic::mytarget_memref_add_rank3),
def MEMREF_RANK3_USE : I<(outs),
  (ins GPR:$a, GPR:$b, GPR:$c, GPR:$d, GPR:$e,
       GPR:$f, GPR:$g, GPR:$h, GPR:$i),
  [(int_mytarget_memref_add_rank3 GPR:$a, GPR:$b, GPR:$c, GPR:$d,
                              GPR:$e, GPR:$f, GPR:$g, GPR:$h,
                              GPR:$i)]>;
