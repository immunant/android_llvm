; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

declare void @llvm.va_start(i8*)
declare void @llvm.va_end(i8*)
declare void @llvm.va_copy(i8*, i8*)

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
; CHECK-NEXT:    call void @llvm.va_end(i8* %1)
; CHECK-NEXT:    ret i32 %2

; CHECK-LABEL: define hidden i32 @"varags$$origva"(i8*)
; CHECK-NEXT:    %va = alloca i8
; CHECK-NEXT:    call void @llvm.va_copy(i8* %va, i8* %0)
; CHECK-NEXT:    %ret = va_arg i8* %va, i32
; CHECK-NEXT:    call void @llvm.va_end(i8* %va)
; CHECK-NEXT:    ret i32 %ret

define i32 @varags(...) {
  %va = alloca i8
  call void @llvm.va_start(i8* %va)
  %ret = va_arg i8* %va, i32
  call void @llvm.va_end(i8* %va)
  ret i32 %ret
}

; CHECK-LABEL: define void @multiple_starts(...)
; CHECK-NEXT:    %1 = alloca i8
; CHECK-NEXT:    call void @llvm.va_start(i8* %1)
; CHECK-NEXT:    call void @"multiple_starts$$origva"(i8* %1)
; CHECK-NEXT:    call void @llvm.va_end(i8* %1)
; CHECK-NEXT:    ret void

; CHECK-LABEL: define hidden void @"multiple_starts$$origva"(i8*)
; CHECK-NEXT:    %va1 = alloca i8
; CHECK-NEXT:    %va2 = alloca i8
; CHECK-NEXT:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK-NEXT:    call void @llvm.va_copy(i8* %va2, i8* %0)
; CHECK-NEXT:    call void @llvm.va_end(i8* %va1)
; CHECK-NEXT:    call void @llvm.va_end(i8* %va2)
; CHECK-NEXT:    ret void

define void @multiple_starts(...) {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_start(i8* %va2)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}

; CHECK-LABEL: define void @copy(...)
; CHECK-NEXT:    %1 = alloca i8
; CHECK-NEXT:    call void @llvm.va_start(i8* %1)
; CHECK-NEXT:    call void @"copy$$origva"(i8* %1)
; CHECK-NEXT:    call void @llvm.va_end(i8* %1)
; CHECK-NEXT:    ret void

; CHECK-LABEL: define hidden void @"copy$$origva"(i8*)
; CHECK-NEXT:    %va1 = alloca i8
; CHECK-NEXT:    %va2 = alloca i8
; CHECK-NEXT:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK-NEXT:    call void @llvm.va_copy(i8* %va2, i8* %va1)
; CHECK-NEXT:    call void @llvm.va_end(i8* %va1)
; CHECK-NEXT:    call void @llvm.va_end(i8* %va2)
; CHECK-NEXT:    ret void

define void @copy(...) {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_copy(i8* %va2, i8* %va1)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}
