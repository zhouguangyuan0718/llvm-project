; RUN: llvm-as < %s >/dev/null

; Ensure rank-expanded memref add intrinsics are callable with memref descriptor
; struct arguments (not flattened leaf argument lists).

declare void @llvm.memref.elem.add.rank1(
  { ptr, ptr, i64, [1 x i64], [1 x i64] },
  { ptr, ptr, i64, [1 x i64], [1 x i64] },
  { ptr, ptr, i64, [1 x i64], [1 x i64] })

declare void @llvm.memref.elem.add.rank2(
  { ptr, ptr, i64, [2 x i64], [2 x i64] },
  { ptr, ptr, i64, [2 x i64], [2 x i64] },
  { ptr, ptr, i64, [2 x i64], [2 x i64] })

define void @test_rank1() {
  call void @llvm.memref.elem.add.rank1(
    { ptr, ptr, i64, [1 x i64], [1 x i64] } poison,
    { ptr, ptr, i64, [1 x i64], [1 x i64] } poison,
    { ptr, ptr, i64, [1 x i64], [1 x i64] } poison)
  ret void
}

define void @test_rank2() {
  call void @llvm.memref.elem.add.rank2(
    { ptr, ptr, i64, [2 x i64], [2 x i64] } poison,
    { ptr, ptr, i64, [2 x i64], [2 x i64] } poison,
    { ptr, ptr, i64, [2 x i64], [2 x i64] } poison)
  ret void
}
