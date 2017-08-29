; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

declare void @llvm.va_start(i8*)
declare void @llvm.va_end(i8*)

define i32 @varags(...) {
  %ap = alloca i8
  call void @llvm.va_start(i8* %ap)
  %ret = va_arg i8* %ap, i32
  call void @llvm.va_end(i8* %ap)
  ret i32 %ret
}

; CHECK-LABEL: define internal void @user()
; CHECK-NEXT:    call i32 (...) @varags(i32 13, i32 37)

define internal void @user() {
  call i32 (...) @varags(i32 13, i32 37)
  ret void
}

; CHECK-LABEL: define i32 @varags(...)
; CHECK-NEXT:    %1 = alloca i8
; CHECK-NEXT:    call void @llvm.va_start(i8* %1)
; CHECK-NEXT:    %2 = call i32 @"varags$$origva"(i8* %1)
; CHECK-NEXT:    ret i32 %2

; CHECK-LABEL: define hidden i32 @"varags$$origva"(i8*)
; CHECK-NEXT:    %ret = va_arg i8* %0, i32
; CHECK-NEXT:    call void @llvm.va_end(i8* %0)
; CHECK-NEXT:    ret i32 %ret
