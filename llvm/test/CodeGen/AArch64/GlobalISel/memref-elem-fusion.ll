; RUN: llc -mtriple=aarch64 -global-isel -global-isel-abort=1 -verify-machineinstrs -stop-after=aarch64-postlegalizer-combiner %s -o - | FileCheck %s

%memref = type { ptr, ptr, i64, [2 x i64], [2 x i64] }

declare void @llvm.memref.elem.add(%memref, %memref, %memref)
declare void @llvm.memref.elem.mul(%memref, %memref, %memref)
declare void @llvm.memref.elem.sub(%memref, %memref, %memref)

define void @mul_add(%memref %a, %memref %b, %memref %c, %memref %tmp,
                     %memref %out) {
; CHECK-LABEL: name:{{ *}}mul_add
; CHECK: G_MEMREF_ELEM_MULADD
; CHECK-NOT: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem
; CHECK: RET_ReallyLR
entry:
  call void @llvm.memref.elem.mul(%memref %a, %memref %b, %memref %tmp)
  call void @llvm.memref.elem.add(%memref %tmp, %memref %c, %memref %out)
  ret void
}

define void @add_mul(%memref %a, %memref %b, %memref %c, %memref %tmp,
                     %memref %out) {
; CHECK-LABEL: name:{{ *}}add_mul
; CHECK: G_MEMREF_ELEM_ADDMUL
; CHECK-NOT: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem
; CHECK: RET_ReallyLR
entry:
  call void @llvm.memref.elem.add(%memref %a, %memref %b, %memref %tmp)
  call void @llvm.memref.elem.mul(%memref %tmp, %memref %c, %memref %out)
  ret void
}

define void @same_block_tmp_observed(%memref %a, %memref %b, %memref %c,
                                     %memref %d, %memref %tmp, %memref %out,
                                     %memref %sink) {
; CHECK-LABEL: name:{{ *}}same_block_tmp_observed
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.mul)
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.add)
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.sub)
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: RET_ReallyLR
entry:
  call void @llvm.memref.elem.mul(%memref %a, %memref %b, %memref %tmp)
  call void @llvm.memref.elem.add(%memref %tmp, %memref %c, %memref %out)
  call void @llvm.memref.elem.sub(%memref %tmp, %memref %d, %memref %sink)
  ret void
}

define void @successor_tmp_observed(i1 %cond, %memref %a, %memref %b,
                                    %memref %c, %memref %d, %memref %tmp,
                                    %memref %out, %memref %sink) {
; CHECK-LABEL: name:{{ *}}successor_tmp_observed
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.mul)
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.add)
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_BRCOND
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: bb.2.after:
; CHECK-NOT: G_MEMREF_ELEM
; CHECK: G_INTRINSIC_W_SIDE_EFFECTS intrinsic(@llvm.memref.flat.elem.sub)
entry:
  call void @llvm.memref.elem.mul(%memref %a, %memref %b, %memref %tmp)
  call void @llvm.memref.elem.add(%memref %tmp, %memref %c, %memref %out)
  br i1 %cond, label %after, label %exit

after:
  call void @llvm.memref.elem.sub(%memref %tmp, %memref %d, %memref %sink)
  br label %exit

exit:
  ret void
}
