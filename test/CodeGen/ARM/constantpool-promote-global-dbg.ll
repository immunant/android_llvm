; RUN: llc -relocation-model=static -arm-promote-constant < %s | FileCheck %s

target datalayout = "e-m:e-p:32:32-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "thumbv7m--linux-gnu"

@const1 = private unnamed_addr constant i32 0, align 4, !dbg !6
@const2 = private unnamed_addr constant i32 0, align 4, !dbg !9

; const1 and const2 both need labels for debug info, but will be coalesced into
; a single constpool entry

; CHECK-LABEL: @test1
; CHECK-DAG: const1:
; CHECK-DAG: const2:
; CHECK: .fnend
define void @test1() #0 {
  %1 = load i32, i32* @const1, align 4
  call void @a(i32 %1) #1
  %2 = load i32, i32* @const2, align 4
  call void @a(i32 %2) #1
  ret void
}

declare void @a(i32) #1

attributes #0 = { nounwind  "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind }

!llvm.dbg.cu = !{!2}
!llvm.ident = !{!20, !20}
!llvm.module.flags = !{!21, !22, !23, !24, !25}

!0 = !DIGlobalVariableExpression(var: !1)
!1 = distinct !DIGlobalVariable(name: "opaque", scope: !2, file: !3, line: 13, type: !8, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "Android clang version 5.0.300080  (based on LLVM 5.0.300080)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, globals: !5)
!3 = !DIFile(filename: "constpool-promote-global-dbg.c", directory: "/test/CodeGen/ARM/")
!4 = !{}
!5 = !{!6, !9}
!6 = !DIGlobalVariableExpression(var: !7)
!7 = distinct !DIGlobalVariable(name: "const1", scope: !2, file: !3, line: 6, type: !8, isLocal: false, isDefinition: true)
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !DIGlobalVariableExpression(var: !10)
!10 = distinct !DIGlobalVariable(name: "const2", scope: !2, file: !3, line: 7, type: !8, isLocal: false, isDefinition: true)
!20 = !{!"Android clang version 5.0.300080  (based on LLVM 5.0.300080)"}
!21 = !{i32 2, !"Dwarf Version", i32 4}
!22 = !{i32 2, !"Debug Info Version", i32 3}
!23 = !{i32 1, !"wchar_size", i32 4}
!24 = !{i32 1, !"min_enum_size", i32 4}
!25 = !{i32 1, !"PIC Level", i32 1}
