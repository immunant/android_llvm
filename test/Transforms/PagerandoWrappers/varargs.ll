; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: %struct.va_list = type { i8*, i8*, i8*, i32, i32 }

declare void @llvm.va_start(i8*)
declare void @llvm.va_end(i8*)
declare void @llvm.va_copy(i8*, i8*)

define internal void @user() {
  call i32 (...) @varags(i32 13, i32 37)
  ret void
}
; CHECK-LABEL: define void @real_va_type(...)
; CHECK:    %1 = alloca %struct.va_list
; CHECK:    %2 = bitcast %struct.va_list* %1 to i8*
; CHECK:    call void @llvm.va_start(i8* %2)
; CHECK:    call void @"real_va_type$$origva"(%struct.va_list* %1)
; CHECK:    %3 = bitcast %struct.va_list* %1 to i8*
; CHECK:    call void @llvm.va_end(i8* %3)
; CHECK:    ret void

; CHECK-LABEL: define void @copy(...)
; CHECK:    %1 = alloca i8
; CHECK:    call void @llvm.va_start(i8* %1)
; CHECK:    call void @"copy$$origva"(i8* %1)
; CHECK:    call void @llvm.va_end(i8* %1)
; CHECK:    ret void

; CHECK-LABEL: define void @multiple_starts(...) 
; CHECK:    %1 = alloca i8
; CHECK:    call void @llvm.va_start(i8* %1)
; CHECK:    call void @"multiple_starts$$origva"(i8* %1)
; CHECK:    call void @llvm.va_end(i8* %1)
; CHECK:    ret void

; CHECK-LABEL: define i32 @varags(...) 
; CHECK:    %1 = alloca i8
; CHECK:    call void @llvm.va_start(i8* %1)
; CHECK:    %2 = call i32 @"varags$$origva"(i8* %1)
; CHECK:    call void @llvm.va_end(i8* %1)
; CHECK:    ret i32 %2

; CHECK-LABEL: define hidden i32 @"varags$$origva"(i8*)
; CHECK:    %va = alloca i8
; CHECK:    call void @llvm.va_copy(i8* %va, i8* %0)
; CHECK:    %ret = va_arg i8* %va, i32
; CHECK:    call void @llvm.va_end(i8* %va)
; CHECK:    ret i32 %ret
define i32 @varags(...) pagerando {
  %va = alloca i8
  call void @llvm.va_start(i8* %va)
  %ret = va_arg i8* %va, i32
  call void @llvm.va_end(i8* %va)
  ret i32 %ret
}

; CHECK-LABEL: define hidden void @"multiple_starts$$origva"(i8*)
; CHECK:    %va1 = alloca i8
; CHECK:    %va2 = alloca i8
; CHECK:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK:    call void @llvm.va_copy(i8* %va2, i8* %0)
; CHECK:    call void @llvm.va_end(i8* %va1)
; CHECK:    call void @llvm.va_end(i8* %va2)
; CHECK:    ret void
define void @multiple_starts(...) pagerando {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_start(i8* %va2)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}

; CHECK-LABEL: define hidden void @"copy$$origva"(i8*)
; CHECK:    %va1 = alloca i8
; CHECK:    %va2 = alloca i8
; CHECK:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK:    call void @llvm.va_copy(i8* %va2, i8* %va1)
; CHECK:    call void @llvm.va_end(i8* %va1)
; CHECK:    call void @llvm.va_end(i8* %va2)
; CHECK:    ret void
define void @copy(...) pagerando {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_copy(i8* %va2, i8* %va1)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}

%struct.va_list = type { i8*, i8*, i8*, i32, i32 }

; CHECK-LABEL: define hidden void @"real_va_type$$origva"(%struct.va_list*)
; CHECK:    %va = alloca %struct.va_list
; CHECK:    %va_ptr = bitcast %struct.va_list* %va to i8*
; CHECK:    %2 = bitcast %struct.va_list* %0 to i8*
; CHECK:    call void @llvm.va_copy(i8* %va_ptr, i8* %2)
; CHECK:    call void @llvm.va_end(i8* %va_ptr)
; CHECK:    ret void
define void @real_va_type(...) pagerando {
  %va = alloca %struct.va_list
  %va_ptr = bitcast %struct.va_list* %va to i8*
  call void @llvm.va_start(i8* %va_ptr)
  call void @llvm.va_end(i8* %va_ptr)
  ret void
}
