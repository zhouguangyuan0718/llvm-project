; REQUIRES: aarch64-registered-target
; RUN: opt -passes='default<O2>' -S < %s | FileCheck %s --check-prefix=OPT
; RUN: llc -mtriple=aarch64-apple-darwin-goobj -verify-machineinstrs \
; RUN:   -enable-shrink-wrap=true -stop-after=prolog-epilog < %s | \
; RUN:   FileCheck %s --check-prefix=MIR
; RUN: llc -mtriple=aarch64-apple-darwin-goobj -verify-machineinstrs \
; RUN:   -enable-shrink-wrap=true -filetype=obj < %s -o %t.o
; RUN: %python %S/../../MC/GoObj/Inputs/dump-goobj.py %t.o | \
; RUN:   FileCheck %s --check-prefix=OBJ

declare void @llvm.go.gc.unsafe.point.start()
declare void @llvm.go.gc.unsafe.point.end(i1 immarg)
declare goabiinternal void @runtime.wbMove(i64, i64, i64)
@runtime.writeBarrier = external global i32

define goabiinternal i64 @main.unsafe_ranges(ptr %p, i64 %v) {
entry:
  %safe0 = load volatile i64, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %v, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  %safe1 = load volatile i64, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %safe0, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  %result = add i64 %safe0, %safe1
  ret i64 %result
}

define goabiinternal i64 @main.same_pc_events(ptr %p, i64 %v) {
entry:
  %safe = load volatile i64, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %v, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %safe, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  ; An optimized empty unsafe interval produces start/end labels at one PC.
  call void @llvm.go.gc.unsafe.point.start()
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  ret i64 %safe
}

define goabiinternal i64 @main.cfg_ranges(ptr %p, i64 %onval, i64 %offval) {
entry:
  call void @llvm.go.gc.unsafe.point.start()
  %flag = load volatile i32, ptr @runtime.writeBarrier, align 4
  %enabled = icmp ne i32 %flag, 0
  call void @llvm.go.gc.unsafe.point.end(i1 true)
  br i1 %enabled, label %on, label %off

on:
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %onval, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 true)
  br label %join

off:
  call void @llvm.go.gc.unsafe.point.start()
  store volatile i64 %offval, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 true)
  br label %join

join:
  call void @llvm.go.gc.unsafe.point.start()
  %joined = load volatile i64, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  %result = add i64 %joined, 1
  ret i64 %result
}

; The write-barrier call makes the on block a natural shrink-wrap candidate.
; Its frame setup must not be inserted before the unsafe marker: a preemption
; there would resume after the write-barrier flag was already read.
define goabiinternal void @main.shrink_wrap_unsafe(ptr %p, ptr %new) {
entry:
  call void @llvm.go.gc.unsafe.point.start()
  %flag = load volatile i32, ptr @runtime.writeBarrier, align 4
  %enabled = icmp ne i32 %flag, 0
  call void @llvm.go.gc.unsafe.point.end(i1 true)
  br i1 %enabled, label %on, label %join

on:
  call void @llvm.go.gc.unsafe.point.start()
  %old = load ptr, ptr %p, align 8
  %p.i = ptrtoint ptr %p to i64
  %new.i = ptrtoint ptr %new to i64
  call goabiinternal void @runtime.wbMove(i64 8, i64 %p.i, i64 %new.i)
  store volatile ptr %old, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 true)
  br label %join

join:
  call void @llvm.go.gc.unsafe.point.start()
  store ptr %new, ptr %p, align 8
  call void @llvm.go.gc.unsafe.point.end(i1 false)
  ret void
}

; OPT-LABEL: define goabiinternal i64 @main.unsafe_ranges
; OPT: load volatile i64
; OPT-NEXT: call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: store volatile i64
; OPT-NEXT: call void @llvm.go.gc.unsafe.point.end(i1 false)

; OPT-LABEL: define goabiinternal i64 @main.same_pc_events
; OPT: call void @llvm.go.gc.unsafe.point.end(i1 false)
; OPT-NEXT: call void @llvm.go.gc.unsafe.point.start()
; OPT: call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: call void @llvm.go.gc.unsafe.point.end(i1 false)

; OPT-LABEL: define goabiinternal i64 @main.cfg_ranges
; OPT: entry:
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: %flag = load volatile i32, ptr @runtime.writeBarrier
; OPT-NEXT: %enabled.not = icmp eq i32 %flag, 0
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.end(i1 true)
; OPT-NEXT: br i1 %enabled.not
; OPT: on:
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: store volatile i64 %onval
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.end(i1 true)
; OPT-NEXT: br label %join
; OPT: off:
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: store volatile i64 %offval
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.end(i1 true)
; OPT-NEXT: br label %join
; OPT: join:
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.start()
; OPT-NEXT: %joined = load volatile i64
; OPT-NEXT: tail call void @llvm.go.gc.unsafe.point.end(i1 false)

; MIR-LABEL: name: main.unsafe_ranges
; MIR: LDRXui
; MIR-NEXT: ANNOTATION_LABEL

; MIR-LABEL: name: main.same_pc_events
; MIR: LDRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: STRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: STRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: ANNOTATION_LABEL

; MIR-LABEL: name: main.cfg_ranges
; MIR: bb.0.entry:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: renamable $x8 = ADRP
; MIR-NEXT: renamable $w8 = LDRWui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: CBZW
; MIR-NEXT: B %bb.1
; MIR: bb.1.on:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: STRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: B %bb.3
; MIR: bb.2.off:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: STRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR: bb.3.join:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: renamable $x8 = LDRXui
; MIR-NEXT: ANNOTATION_LABEL
; MIR-NEXT: $x0 = ADDXri

; MIR-LABEL: name: main.shrink_wrap_unsafe
; MIR: body:
; MIR: bb.0.entry:
; MIR: frame-setup STRXpre
; MIR: ANNOTATION_LABEL
; MIR-NEXT: renamable $x8 = ADRP
; MIR: bb.1.on:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: renamable $x8 = LDRXui
; MIR: BL @runtime.wbMove
; MIR: bb.2.join:
; MIR: ANNOTATION_LABEL
; MIR-NEXT: STRXui

; OBJ: symdef 0: main.unsafe_ranges
; OBJ: symdef 3: main.shrink_wrap_unsafe
; OBJ: aux 0.6: type=pcdata target= pc=[0-1:-1,1-2:-2,2-3:-1,3-4:-2,4-6:-1]
; OBJ: aux 1.14: type=pcdata target= pc=[0-1:-1,1-3:-2,3-5:-1]
; OBJ: aux 2.22: type=pcdata target= pc=[0-7:-2,7-9:-1]
; OBJ: aux 3.30: type=pcdata target= pc=[0-7:-1,7-21:-2,21-31:-1]
