; RUN: llc -mtriple=x86_64-unknown-linux-goobj -verify-machineinstrs \
; RUN:   -stop-after=prolog-epilog < %s | FileCheck %s

define goabiinternal i64 @morestack_statepoint(i64 %value) "go-stack-growth-statepoint" {
entry:
  %buf = alloca [5000 x i8], align 16
  %slot = getelementptr inbounds [5000 x i8], ptr %buf, i64 0, i64 4999
  store volatile i8 1, ptr %slot, align 1
  ret i64 %value
}

define goabiinternal ptr @stack_pointer_morestack_statepoint(
    i64 %a0, i64 %a1, i64 %a2, i64 %a3, i64 %a4,
    i64 %a5, i64 %a6, i64 %a7, i64 %a8,
    ptr byval(ptr) align 8 %pointer.byval)
    "go-stack-growth-statepoint" {
entry:
  %buf = alloca [5000 x i8], align 16
  %slot = getelementptr inbounds [5000 x i8], ptr %buf, i64 0, i64 4999
  store volatile i8 1, ptr %slot, align 1
  %pointer = load ptr, ptr %pointer.byval, align 8
  ret ptr %pointer
}

; CHECK-LABEL: name: morestack_statepoint
; CHECK-NOT: ANNOTATION_LABEL
; CHECK: STATEPOINT 5147424658422983495, 0, 0, &runtime.morestack_noctxt,
; CHECK-SAME: 2, 22, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0,
; CHECK-SAME: csr_64_go, implicit-def $rsp, implicit-def $ssp
; CHECK-NOT: CALL64pcrel32

; CHECK-LABEL: name: stack_pointer_morestack_statepoint
; CHECK: STATEPOINT 5147424658422983495, 0, 0, &runtime.morestack_noctxt,
; CHECK-SAME: 2, 22, 2, 0, 2, 0, 2, 1,
; CHECK-SAME: 1, 8, $rsp, 8,
; CHECK-SAME: 2, 0, 2, 1, 0, 0,
; CHECK-SAME: csr_64_go, implicit-def $rsp, implicit-def $ssp
