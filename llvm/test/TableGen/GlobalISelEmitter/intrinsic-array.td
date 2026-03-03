// RUN: llvm-tblgen -gen-global-isel -optimize-match-table=false -I %p/../Common -I %p/../../../include %s | FileCheck -check-prefix=GISEL %s

include "llvm/Target/Target.td"
include "GlobalISelEmitterCommon.td"

let TargetPrefix = "mytarget" in {
def int_mytarget_array2 : Intrinsic<[], [LLVMArrayType<2, llvm_i32_ty>]>;
}

// GISEL: GIM_CheckOpcode, /*MI*/0, GIMT_Encode2(TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS),
// GISEL-NEXT: // MIs[0] Operand 0
// GISEL-NEXT: GIM_CheckIntrinsicID, /*MI*/0, /*Op*/0, GIMT_Encode2(Intrinsic::mytarget_array2),
// GISEL-NEXT: // MIs[0] a
// GISEL-NEXT: GIM_RecordNamedOperand, /*MI*/0, /*OpIdx*/1, /*StoreIdx=*/{{[0-9]+}},
// GISEL-NEXT: // MIs[0] b
// GISEL-NEXT: GIM_RecordNamedOperand, /*MI*/0, /*OpIdx*/2, /*StoreIdx=*/{{[0-9]+}},
// GISEL: // (intrinsic_void {{[0-9]+}}:{ *:[iPTR] }, i32:$a, i32:$b)  =>  (ARR2 i32:$a, i32:$b)
// GISEL-NEXT: GIR_BuildRootMI, /*Opcode*/GIMT_Encode2(MyTarget::ARR2),
// GISEL-NEXT: GIR_RootToRootCopy, /*OpIdx*/0, // a
// GISEL-NEXT: GIR_RootToRootCopy, /*OpIdx*/1, // b
def ARR2 : I<(outs), (ins GPR32:$a, GPR32:$b),
  [(int_mytarget_array2 GPR32:$a, GPR32:$b)]>;
