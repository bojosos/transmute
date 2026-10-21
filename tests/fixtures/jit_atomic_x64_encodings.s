.text
.intel_syntax noprefix
lock xadd byte ptr [rdx], r8b
lock xadd word ptr [rdx], r8w
lock xadd dword ptr [rdx], r8d
lock xadd qword ptr [rdx], r8
xchg byte ptr [rdx], r8b
xchg word ptr [rdx], r8w
xchg dword ptr [rdx], r8d
xchg qword ptr [rdx], r8
lock cmpxchg byte ptr [rdx], r8b
lock cmpxchg word ptr [rdx], r8w
lock cmpxchg dword ptr [rdx], r8d
lock cmpxchg qword ptr [rdx], r8
cmp al, r9b
cmp ax, r9w
cmp eax, r9d
cmp rax, r9
cmovge r8d, eax
cmovle r8d, eax
cmovae r8d, eax
cmovbe r8d, eax
sete r9b
movzx r9d, r9b
mov r8d, r9d
not r8d
and r8d, eax
xor r8d, r9d
or r8d, r9d
