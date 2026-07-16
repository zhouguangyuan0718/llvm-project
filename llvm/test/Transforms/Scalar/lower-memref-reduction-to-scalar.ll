; RUN: split-file %s %t
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/add-1d.ll -S | FileCheck %s --check-prefix=ADD1D
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/max-1d-strided.ll -S | FileCheck %s --check-prefix=MAX1D-STRIDED
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/add-2d-1x8.ll -S | FileCheck %s --check-prefix=ADD2D-1X8
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/max-2d-8x1.ll -S | FileCheck %s --check-prefix=MAX2D-8X1
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/single-element.ll -S | FileCheck %s --check-prefix=SINGLE
; RUN: opt -passes='function(lower-memref-reduction-to-scalar)' %t/invalid-dim.ll -S | FileCheck %s --check-prefix=INVALID

; This test uses separate modules because the input and output memref descriptor
; ranks may differ between cases.

;--- add-1d.ll
%memref1d = type { ptr, ptr, i64, [1 x i64], [1 x i64] }

declare void @llvm.memref.reduce.add(%memref1d, %memref1d, i64)

define void @reduce_add_1d(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref1d poison, ptr %input, 0
  %in1 = insertvalue %memref1d %in0, ptr %input, 1
  %in2 = insertvalue %memref1d %in1, i64 0, 2
  %in3 = insertvalue %memref1d %in2, i64 8, 3, 0
  %in4 = insertvalue %memref1d %in3, i64 1, 4, 0

  %out0 = insertvalue %memref1d poison, ptr %output, 0
  %out1 = insertvalue %memref1d %out0, ptr %output, 1
  %out2 = insertvalue %memref1d %out1, i64 0, 2
  %out3 = insertvalue %memref1d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref1d %out3, i64 1, 4, 0

  call void @llvm.memref.reduce.add(%memref1d %in4, %memref1d %out4, i64 0)
  ret void
}

; ADD1D-LABEL: define void @reduce_add_1d(
; ADD1D: %reduction.initial.acc = load half
; ADD1D: br label %memref.reduction.loop
; ADD1D: memref.reduction.loop:
; ADD1D: %reduction.iv = phi i64 [ 1, %entry ], [ %reduction.next.iv, %memref.reduction.loop ]
; ADD1D: %reduction.acc = phi half
; ADD1D: %reduction.elem = load half
; ADD1D: %reduction.next.acc = fadd half %reduction.acc, %reduction.elem
; ADD1D: %reduction.next.iv = add i64 %reduction.iv, 1
; ADD1D: %reduction.done = icmp eq i64 %reduction.next.iv, 8
; ADD1D: memref.reduction.exit:
; ADD1D: store half %reduction.next.acc
; ADD1D-NOT: call void @llvm.memref.reduce.add

;--- max-1d-strided.ll
%memref1d = type { ptr, ptr, i64, [1 x i64], [1 x i64] }

declare void @llvm.memref.reduce.max(%memref1d, %memref1d, i64)

define void @reduce_max_1d_strided(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref1d poison, ptr %input, 0
  %in1 = insertvalue %memref1d %in0, ptr %input, 1
  %in2 = insertvalue %memref1d %in1, i64 3, 2
  %in3 = insertvalue %memref1d %in2, i64 8, 3, 0
  %in4 = insertvalue %memref1d %in3, i64 2, 4, 0

  %out0 = insertvalue %memref1d poison, ptr %output, 0
  %out1 = insertvalue %memref1d %out0, ptr %output, 1
  %out2 = insertvalue %memref1d %out1, i64 0, 2
  %out3 = insertvalue %memref1d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref1d %out3, i64 1, 4, 0

  call void @llvm.memref.reduce.max(%memref1d %in4, %memref1d %out4, i64 0)
  ret void
}

; MAX1D-STRIDED-LABEL: define void @reduce_max_1d_strided(
; MAX1D-STRIDED: %reduction.initial.acc = load half
; MAX1D-STRIDED: memref.reduction.loop:
; MAX1D-STRIDED: %reduction.elem.delta = mul i64 %reduction.iv, 2
; MAX1D-STRIDED: %reduction.elem.offset = add i64 {{.*}}, %reduction.elem.delta
; MAX1D-STRIDED: %reduction.elem = load half
; MAX1D-STRIDED: %reduction.next.acc = call half @llvm.maximum.f16(half %reduction.acc, half %reduction.elem)
; MAX1D-STRIDED: store half %reduction.next.acc
; MAX1D-STRIDED-NOT: call void @llvm.memref.reduce.max

;--- add-2d-1x8.ll
%memref1d = type { ptr, ptr, i64, [1 x i64], [1 x i64] }
%memref2d = type { ptr, ptr, i64, [2 x i64], [2 x i64] }

declare void @llvm.memref.reduce.add(%memref2d, %memref1d, i64)

define void @reduce_add_2d_1x8(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref2d poison, ptr %input, 0
  %in1 = insertvalue %memref2d %in0, ptr %input, 1
  %in2 = insertvalue %memref2d %in1, i64 0, 2
  %in3 = insertvalue %memref2d %in2, i64 1, 3, 0
  %in4 = insertvalue %memref2d %in3, i64 8, 3, 1
  %in5 = insertvalue %memref2d %in4, i64 8, 4, 0
  %in6 = insertvalue %memref2d %in5, i64 1, 4, 1

  %out0 = insertvalue %memref1d poison, ptr %output, 0
  %out1 = insertvalue %memref1d %out0, ptr %output, 1
  %out2 = insertvalue %memref1d %out1, i64 0, 2
  %out3 = insertvalue %memref1d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref1d %out3, i64 1, 4, 0

  call void @llvm.memref.reduce.add(%memref2d %in6, %memref1d %out4, i64 1)
  ret void
}

; ADD2D-1X8-LABEL: define void @reduce_add_2d_1x8(
; ADD2D-1X8: %reduction.initial.acc = load half
; ADD2D-1X8: memref.reduction.loop:
; ADD2D-1X8-NOT: %reduction.elem.delta = mul
; ADD2D-1X8: %reduction.elem = load half
; ADD2D-1X8: %reduction.next.acc = fadd half %reduction.acc, %reduction.elem
; ADD2D-1X8: %reduction.done = icmp eq i64 %reduction.next.iv, 8
; ADD2D-1X8: store half %reduction.next.acc
; ADD2D-1X8-NOT: call void @llvm.memref.reduce.add

;--- max-2d-8x1.ll
%memref2d = type { ptr, ptr, i64, [2 x i64], [2 x i64] }

declare void @llvm.memref.reduce.max(%memref2d, %memref2d, i64)

define void @reduce_max_2d_8x1(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref2d poison, ptr %input, 0
  %in1 = insertvalue %memref2d %in0, ptr %input, 1
  %in2 = insertvalue %memref2d %in1, i64 0, 2
  %in3 = insertvalue %memref2d %in2, i64 8, 3, 0
  %in4 = insertvalue %memref2d %in3, i64 1, 3, 1
  %in5 = insertvalue %memref2d %in4, i64 1, 4, 0
  %in6 = insertvalue %memref2d %in5, i64 1, 4, 1

  %out0 = insertvalue %memref2d poison, ptr %output, 0
  %out1 = insertvalue %memref2d %out0, ptr %output, 1
  %out2 = insertvalue %memref2d %out1, i64 0, 2
  %out3 = insertvalue %memref2d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref2d %out3, i64 1, 3, 1
  %out5 = insertvalue %memref2d %out4, i64 1, 4, 0
  %out6 = insertvalue %memref2d %out5, i64 1, 4, 1

  call void @llvm.memref.reduce.max(%memref2d %in6, %memref2d %out6, i64 0)
  ret void
}

; MAX2D-8X1-LABEL: define void @reduce_max_2d_8x1(
; MAX2D-8X1: %reduction.initial.acc = load half
; MAX2D-8X1: memref.reduction.loop:
; MAX2D-8X1: %reduction.next.acc = call half @llvm.maximum.f16(half %reduction.acc, half %reduction.elem)
; MAX2D-8X1: %reduction.done = icmp eq i64 %reduction.next.iv, 8
; MAX2D-8X1: store half %reduction.next.acc
; MAX2D-8X1-NOT: call void @llvm.memref.reduce.max

;--- single-element.ll
%memref1d = type { ptr, ptr, i64, [1 x i64], [1 x i64] }

declare void @llvm.memref.reduce.max(%memref1d, %memref1d, i64)

define void @reduce_max_single_element(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref1d poison, ptr %input, 0
  %in1 = insertvalue %memref1d %in0, ptr %input, 1
  %in2 = insertvalue %memref1d %in1, i64 0, 2
  %in3 = insertvalue %memref1d %in2, i64 1, 3, 0
  %in4 = insertvalue %memref1d %in3, i64 1, 4, 0

  %out0 = insertvalue %memref1d poison, ptr %output, 0
  %out1 = insertvalue %memref1d %out0, ptr %output, 1
  %out2 = insertvalue %memref1d %out1, i64 0, 2
  %out3 = insertvalue %memref1d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref1d %out3, i64 1, 4, 0

  call void @llvm.memref.reduce.max(%memref1d %in4, %memref1d %out4, i64 0)
  ret void
}

; SINGLE-LABEL: define void @reduce_max_single_element(
; SINGLE: %reduction.result = load half
; SINGLE-NEXT: store half %reduction.result
; SINGLE-NOT: memref.reduction.loop
; SINGLE-NOT: @llvm.maximum
; SINGLE-NOT: call void @llvm.memref.reduce.max

;--- invalid-dim.ll
%memref1d = type { ptr, ptr, i64, [1 x i64], [1 x i64] }
%memref2d = type { ptr, ptr, i64, [2 x i64], [2 x i64] }

declare void @llvm.memref.reduce.add(%memref2d, %memref1d, i64)

define void @invalid_reduce_dim(ptr %input, ptr %output) {
entry:
  %in0 = insertvalue %memref2d poison, ptr %input, 0
  %in1 = insertvalue %memref2d %in0, ptr %input, 1
  %in2 = insertvalue %memref2d %in1, i64 0, 2
  %in3 = insertvalue %memref2d %in2, i64 1, 3, 0
  %in4 = insertvalue %memref2d %in3, i64 8, 3, 1
  %in5 = insertvalue %memref2d %in4, i64 8, 4, 0
  %in6 = insertvalue %memref2d %in5, i64 1, 4, 1

  %out0 = insertvalue %memref1d poison, ptr %output, 0
  %out1 = insertvalue %memref1d %out0, ptr %output, 1
  %out2 = insertvalue %memref1d %out1, i64 0, 2
  %out3 = insertvalue %memref1d %out2, i64 1, 3, 0
  %out4 = insertvalue %memref1d %out3, i64 1, 4, 0

  call void @llvm.memref.reduce.add(%memref2d %in6, %memref1d %out4, i64 0)
  ret void
}

; INVALID-LABEL: define void @invalid_reduce_dim(
; INVALID: call void @llvm.memref.reduce.add(%memref2d %in6, %memref1d %out4, i64 0)
; INVALID-NOT: memref.reduction.loop
