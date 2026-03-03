; RUN: llvm-as < %s >/dev/null

; Ensure rank-expanded memref add intrinsic declarations are representable in IR.
; Signature shape for rank1 memref descriptor:
;   (ptr, ptr, i64, i64, i64)
; Three memref args (src0, src1, dst) => 15 operands.
declare void @llvm.mytarget.memref.add.rank1(
  ptr, ptr, i64, i64, i64,
  ptr, ptr, i64, i64, i64,
  ptr, ptr, i64, i64, i64)

; Signature shape for rank2 memref descriptor:
;   (ptr, ptr, i64, i64, i64, i64, i64)
; Three memref args (src0, src1, dst) => 21 operands.
declare void @llvm.mytarget.memref.add.rank2(
  ptr, ptr, i64, i64, i64, i64, i64,
  ptr, ptr, i64, i64, i64, i64, i64,
  ptr, ptr, i64, i64, i64, i64, i64)

define void @test_rank1() {
  call void @llvm.mytarget.memref.add.rank1(
    ptr null, ptr null, i64 0, i64 4, i64 1,
    ptr null, ptr null, i64 0, i64 4, i64 1,
    ptr null, ptr null, i64 0, i64 4, i64 1)
  ret void
}

define void @test_rank2() {
  call void @llvm.mytarget.memref.add.rank2(
    ptr null, ptr null, i64 0, i64 4, i64 8, i64 8, i64 1,
    ptr null, ptr null, i64 0, i64 4, i64 8, i64 8, i64 1,
    ptr null, ptr null, i64 0, i64 4, i64 8, i64 8, i64 1)
  ret void
}
