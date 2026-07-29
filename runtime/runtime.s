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
  cmp dword [rdi], 128
  jae .skip_fits

  push rbx
  push r10

  mov rbx, 0

.loop_begin:
  cmp ebx, [rdi]
  jae .loop_end

  lea r10, [rdi+4]
  mov r10b, [r10+rbx]
  mov [stdout_buf+rbx], r10b
  inc ebx

  jmp .loop_begin
.loop_end:

  mov byte [stdout_buf+rbx], 10
  inc rbx

  mov edx, ebx
  lea rsi, [stdout_buf]
  mov rdi, 1
  mov rax, 1
  syscall

  pop r10
  pop rbx
  ret
.skip_fits:
  mov edx, [rdi]
  lea rsi, [rdi+4]
  mov rdi, 1
  mov rax, 1
  syscall

  mov edx, 1
  lea rsi, [new_line]
  mov rdi, 1
  mov rax, 1
  syscall

  ret

len_internal:
  cmp rdi, 0
  jne .skip_null
  mov rax, rsi
  ret
.skip_null:

  mov rdi, [rdi]
  inc rsi
  jmp len_internal

global len
len:
  mov rsi, 0
  jmp len_internal

global ether_get_value_len_as_str_2 ; int
ether_get_value_len_as_str_2:
  cmp rdi, 0
  jne .skip_zero
  mov rax, 1
  ret
.skip_zero:

  push rbx
  push r10

  mov rbx, 0

  cmp rdi, 0
  jge .skip_neg
  inc rbx
  mov rax, rdi
  mov r10, -1
  imul r10
  mov rdi, rax
.skip_neg:

  mov r10, 10

.loop_begin:
  cmp rdi, 0
  je .loop_end

  mov rax, rdi
  xor rdx, rdx
  div r10
  mov rdi, rax
  inc rbx

  jmp .loop_begin
.loop_end:

  mov rax, rbx
  pop r10
  pop rbx
  ret

global ether_get_value_len_as_str_3 ; bool
ether_get_value_len_as_str_3:
  cmp rdi, 0
  je .skip_true
  mov rax, 4
  ret
.skip_true:
  mov rax, 5
  ret

global ether_get_value_len_as_str_4 ; str
ether_get_value_len_as_str_4:
  mov eax, [rdi]
  ret

global ether_get_value_len_as_str_4_quoted ; str
ether_get_value_len_as_str_4_quoted:
  mov eax, [rdi]
  add eax, 2
  ret

global ether_alloc
ether_alloc:
  push rbx
  push r10

  add rdi, 8

  mov r9, 0
  mov r8, -1
  mov r10, 34
  mov rdx, 3
  mov rsi, rdi
  mov rdi, 0
  mov rax, 9
  syscall

  mov rbx, 0
.loop_begin:
  cmp rbx, rsi
  jae .loop_end

  mov byte [rax+rbx], 0
  inc rbx

  jmp .loop_begin
.loop_end:

  add rax, 8

  pop r10
  pop rbx
  ret

global ether_free
ether_free:
  sub rdi, 8
  mov rax, 11
  syscall

  ret

ether_free_4:; str
  mov rsi, [rdi+8]
  add rsi, 6
  mov rax, 11
  syscall

  ret

global ether_rc_inc_4 ; str
ether_rc_inc_4:
  mov rsi, rdi
  mov edx, [rdi]
  add rsi, rdx
  add rsi, 5
  cmp byte [rsi], 1
  je .skip_static
  inc qword [rdi-8]
.skip_static:

  ret

global ether_rc_dec_4 ; str
ether_rc_dec_4:
  mov rsi, rdi
  mov edx, [rdi]
  add rsi, rdx
  add rsi, 5
  cmp byte [rsi], 1
  je .skip_static
  dec qword [rdi-8]
  cmp qword[rdi-8], 0
  jle ether_free_4
.skip_static:

  ret

global ether_rc_inc_5 ; list
ether_rc_inc_5:
  cmp rdi, 0
  je .skip_null
  inc qword [rdi-8]
.skip_null:

  ret

; ether_rc_dec_5 (for lists) is generated because it has to be generic

global ether_value_to_str_2 ; int
ether_value_to_str_2:
  cmp rdx, 0
  jne .skip_zero
  mov byte [rdi+rsi], '0'
  mov rax, 1
  ret
.skip_zero:

  push rbx
  push r10
  push r11
  push r12

  mov rbx, rdi
  mov rdi, rdx
  mov r10, rdx
  call ether_get_value_len_as_str_2
  mov rdx, r10
  mov rdi, rbx
  dec rax

  add rdi, rsi
  add rdi, rax
  mov r11, rdx
  mov rbx, 0

  mov r12, 0
  cmp r11, 0
  jge .skip_neg0
  mov r12, 1
  mov rax, r11
  mov r10, -1
  imul r10
  mov r11, rax
.skip_neg0:

  mov r10, 10

.loop_begin:
  cmp r11, 0
  je .loop_end

  mov rax, r11
  xor rdx, rdx
  div r10
  mov r11, rax
  mov byte [rdi], '0'
  add [rdi], dl
  inc rbx
  dec rdi

  jmp .loop_begin
.loop_end:

  cmp r12, 0
  je .skip_neg1
  mov byte [rdi], '-'
  inc rbx
.skip_neg1:

  mov rax, rbx
  pop r12
  pop r11
  pop r10
  pop rbx
  ret

global ether_value_to_str_3 ; bool
ether_value_to_str_3:
  add rdi, rsi
  cmp rdx, 0
  je .skip_true
  mov byte [rdi+0], 't'
  mov byte [rdi+1], 'r'
  mov byte [rdi+2], 'u'
  mov byte [rdi+3], 'e'
  mov rax, 4
  ret
.skip_true:
  mov byte [rdi+0], 'f'
  mov byte [rdi+1], 'a'
  mov byte [rdi+2], 'l'
  mov byte [rdi+3], 's'
  mov byte [rdi+4], 'e'
  mov rax, 5
  ret

global ether_value_to_str_4 ; str
ether_value_to_str_4:
  push rbx
  push r10
  push r11

  add rdi, rsi
  mov r11d, [rdx]
  add rdx, 4

  mov rbx, 0

.loop_begin:
  cmp ebx, r11d
  jae .loop_end

  mov r10b, [rdx]
  mov [rdi+rbx], r10b
  inc rbx
  inc rdx

  jmp .loop_begin
.loop_end:

  mov rax, rbx
  pop r11
  pop r10
  pop rbx
  ret

global ether_value_to_str_4_quoted ; str
ether_value_to_str_4_quoted:
  push rbx
  push r10

  mov byte [rdi+rsi], 39
  inc rsi
  mov rbx, rdi
  mov r10, rsi
  call ether_value_to_str_4
  add r10, rax
  mov byte [rbx+r10], 39

  add rax, 2
  pop r10
  pop rbx
  ret

section '.data'
new_line: db 10
section '.bss'
stdout_buf: resb 128
