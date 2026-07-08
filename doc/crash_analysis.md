# mod_myasr 崩溃分析报告

## 问题描述

FreeSWITCH 的 `mod_myasr` 模块在运行时发生崩溃重启，崩溃发生在 `wss_client_run` 线程内部。

**一句话结论：**

> WSS 线程持有 `WsUserData *wud` 指针期间，主线程在通话结束时提前释放了该内存，导致 `ws_client.run()` 返回后线程访问 `wud->bufidx` 时触发 use-after-free 崩溃。

---

## GDB 分析过程

### 1. 初始 backtrace

```
(gdb) bt
#0  0x00007fba42ea2152 in wss_client_run(void*) () from /opt/freeswitch/lib/freeswitch/mod/mod_myasr.so
#1  0x00007fba6bbabea5 in start_thread () from /lib64/libpthread.so.0
#2  0x00007fba6b1ffb0d in clone () from /lib64/libc.so.6
```

崩溃发生在 `wss_client_run` 函数内部，但 .so 没有调试符号，无法直接看局部变量。

### 2. 寄存器状态

```
rax   = 0x293c9d0       ← wud 指针（WsUserData*）
rsi   = 0x0             ← NULL（可疑）
rdi   = 0x7fb988cc8220  ← 线程参数（栈上临时地址）
rip   = wss_client_run+1413  ← 崩溃指令位置
```

### 3. 崩溃点汇编分析

```asm
0x7fba42ea2149 <+1404>:   callq  _ZN9wssclient3runEv     ; ws_client.run() 调用
0x7fba42ea214e <+1409>:   mov    -0x28(%rbp), %rax       ; rax = wud
=> 0x7fba42ea2152 <+1413>:   mov    0x54(%rax), %eax    ; 读取 wud->bufidx ← CRASH
```

`rbp-0x28` 存的是 `wud`（即 `void *arg` 转型后的 `WsUserData*`），崩溃发生在 `ws_client.run()` 返回后立即访问 `wud->bufidx`。

### 4. 确认 wud 内存已释放

```
(gdb) x/16xg 0x293c9d0
Cannot access memory at address 0x293c9d0
```

`wud` 指向的内存已彻底不可访问，已归还给操作系统。

---

## 根本原因分析

### WsUserData 结构体 offset 0x54 对应字段

```cpp
struct WsUserData {
    char uuid[64];       // offset 0x00
    char leg[10];        // offset 0x40
    // 2字节对齐填充
    int sendcounter;     // offset 0x4c
    int recvcounter;     // offset 0x50
    int bufidx;          // offset 0x54  ← 崩溃访问的字段
    time_t timeout;      // offset 0x58
    int sendFlag;        // offset 0x60
    int connected;       // offset 0x64
    char custom_appid[64]; // offset 0x68
};
```

### 崩溃时序

```
主线程                          WSS 线程 (wss_client_run)
─────────────────────────────────────────────────────────
pthread_create(..., wss_client_run, aleg_wud)
                                wud = (WsUserData *)arg
                                ws_client.run()  ← 阻塞（WebSocket 事件循环）
通话结束
free(aleg_wud)    ← ★ wud 内存被释放
                                ws_client.run() 返回
                                wud->bufidx      ← ★ 访问已释放内存 → SIGSEGV
```

### 问题本质

- WSS 线程**持有指针但不拥有生命周期**
- 主线程**拥有内存但不感知线程状态**
- 二者之间**缺乏同步机制**，是典型的多线程 dangling pointer 问题

---

## 修复方案

### 快速修复：值拷贝（推荐）

在 `wss_client_run` 函数开头，立即将需要用到的字段拷贝到栈上局部变量，与堆对象的生命周期解耦：

```cpp
static void *wss_client_run(void *arg)
{
    struct WsUserData *wud = (struct WsUserData *)arg;
    if (!wud) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client get param null\n");
        pthread_exit(NULL);
        return 0;
    }

    // ★ 立即拷贝关键字段到本地，之后不再依赖 wud 指针存活
    const int local_bufidx = wud->bufidx;

    // ... 原有代码不变 ...

    // ★ ws_client.run() 返回后，所有日志改用 local_bufidx
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " wss client end bufidx=%d\n", local_bufidx);

    // catch 块同样使用 local_bufidx
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "wss_client_run thread exit, bufidx=%d\n", local_bufidx);
    pthread_exit(NULL);
    return 0;
}
```

**涉及修改的行：** `mod_myasr.cpp` 第 1326、1328、1331、1341、1352 行。

### 根本修复：生命周期同步

确保在 WSS 线程退出之前，主线程不释放 `wud`：

```cpp
// 创建线程时保存 tid
pthread_create(&wss_thread, &attr, wss_client_run, aleg_wud);

// 通话结束时，先等待 WSS 线程退出，再释放 wud
pthread_join(wss_thread, NULL);
free(aleg_wud);  // 此时安全
```

---

## 总结

| 项目 | 内容 |
|------|------|
| 崩溃类型 | Use-After-Free (SIGSEGV) |
| 崩溃位置 | `wss_client_run` +1413，`wud->bufidx` 读取 |
| 触发条件 | 通话在 WebSocket 连接期间结束 |
| GDB 确认 | `Cannot access memory at address 0x293c9d0` |
| 快速修复 | 函数开头拷贝 `bufidx` 到局部变量 |
| 根本修复 | `pthread_join` 保证内存释放顺序 |
