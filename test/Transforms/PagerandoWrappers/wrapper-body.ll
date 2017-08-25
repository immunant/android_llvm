; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

define i32 @global_fn_body(i32 %arg) {
  ret i32 %arg
}

; CHECK-LABEL: define hidden i32 @"global_fn_body$$orig"(i32 %arg)
; CHECK-NEXT:    ret i32 %arg

; CHECK-LABEL: define i32 @global_fn_body(i32)
; CHECK-NEXT:    %2 = call i32 @"global_fn_body$$orig"(i32 %0)
; CHECK-NEXT:    ret i32 %2