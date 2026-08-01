; REQUIRES: aarch64-registered-target
; RUN: split-file %s %t
; RUN: not --crash llc -mtriple=aarch64-apple-darwin-goobj -filetype=obj \
; RUN:   < %t/unbalanced.ll -o /dev/null 2>&1 | \
; RUN:   FileCheck %s --check-prefix=UNBALANCED
; RUN: not --crash llc -mtriple=aarch64-apple-darwin-goobj -filetype=obj \
; RUN:   < %t/overlap.ll -o /dev/null 2>&1 | \
; RUN:   FileCheck %s --check-prefix=OVERLAP

;--- unbalanced.ll
declare void @llvm.go.gc.unsafe.point.start()

define goabiinternal void @main.unbalanced() {
entry:
  call void @llvm.go.gc.unsafe.point.start()
  ret void
}

; UNBALANCED: LLVM ERROR: GoObj unsafe-point range has no end marker

;--- overlap.ll
declare void @llvm.go.gc.unsafe.point.start()
declare void @llvm.go.gc.unsafe.point.end(i1 immarg)

define goabiinternal void @main.overlap() {
entry:
  call void @llvm.go.gc.unsafe.point.start()
  call void @llvm.go.gc.unsafe.point.start()
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  ret void
}

; OVERLAP: LLVM ERROR: GoObj unsafe-point ranges overlap
