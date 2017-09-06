; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: define hidden i32 @"foo$$orig"(i32 %arg) #0
; CHECK-NEXT:    ret i32 %arg

define i32 @foo(i32 %arg) {
  ret i32 %arg
}

; CHECK-LABEL: define i32 @foo(i32) #1
; CHECK-NEXT:    %2 = call i32 @"foo$$orig"(i32 %0)
; CHECK-NEXT:    ret i32 %2

; CHECK-LABEL: attributes #0 = { pagerando }
; CHECK-LABEL: attributes #1 = { noinline optsize }
