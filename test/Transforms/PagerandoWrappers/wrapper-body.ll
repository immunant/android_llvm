; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: define i32 @foo(i32) #0
; CHECK-NEXT:    %2 = call i32 @"foo$$orig"(i32 %0)
; CHECK-NEXT:    ret i32 %2

define i32 @foo(i32 %arg) pagerando {
  ret i32 %arg
}

; CHECK-LABEL: define hidden i32 @"foo$$orig"(i32 %arg) #1
; CHECK-NEXT:    ret i32 %arg

; CHECK-LABEL: attributes #0 = { noinline optsize }
; CHECK-LABEL: attributes #1 = { pagerando }