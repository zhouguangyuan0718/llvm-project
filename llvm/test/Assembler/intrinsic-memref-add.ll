; RUN: llvm-as < %s >/dev/null

; Ensure rank-expanded memref add intrinsics are callable with memref descriptor
; struct arguments (not flattened leaf argument lists).

%mref_rank1_t = type { ptr, ptr, i64, i64, i64 }
%mref_rank2_t = type { ptr, ptr, i64, [2 x i64], [2 x i64] }

declare void @llvm.mytarget.memref.add.rank1(
  %mref_rank1_t, %mref_rank1_t, %mref_rank1_t)

declare void @llvm.mytarget.memref.add.rank2(
  %mref_rank2_t, %mref_rank2_t, %mref_rank2_t)

define void @test_rank1() {
  call void @llvm.mytarget.memref.add.rank1(
    %mref_rank1_t poison,
    %mref_rank1_t poison,
    %mref_rank1_t poison)
  ret void
}

define void @test_rank2() {
  call void @llvm.mytarget.memref.add.rank2(
    %mref_rank2_t poison,
    %mref_rank2_t poison,
    %mref_rank2_t poison)
  ret void
}
