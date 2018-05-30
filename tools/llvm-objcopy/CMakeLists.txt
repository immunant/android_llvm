set(LLVM_LINK_COMPONENTS
  Object
  Support
  MC
  )
add_llvm_tool(llvm-objcopy
  llvm-objcopy.cpp
  Object.cpp
  )

add_llvm_tool_symlink(llvm-strip llvm-objcopy)

if(LLVM_INSTALL_BINUTILS_SYMLINKS)
  add_llvm_tool_symlink(objcopy llvm-objcopy)
  add_llvm_tool_symlink(strip llvm-objcopy)
endif()
