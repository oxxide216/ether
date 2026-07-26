section '.text'
global print
print:
  mov edx, [rdi]
  lea rsi, [rdi+4]
  mov rdi, 1
  mov rax, 1
  syscall

  ret

global println
println:
  mov rbx, rdi

  mov edx, [rbx]
  lea rsi, [rbx+4]
  mov rdi, 1
  mov rax, 1
  syscall

  mov edx, 1
  lea rsi, [new_line]
  mov rdi, 1
  mov rax, 1
  syscall

  ret

section '.data'
new_line: db 10
