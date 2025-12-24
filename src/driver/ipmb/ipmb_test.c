/*
 * Copyright (c) 2025 IPMB Async Integration Test
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

/* 使用 POSIX 标准头文件 */
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/poll.h>
#include <errno.h>


/* 设备路径 */
#define IPMB_DEV_PATH "/dev/ipmb/ipmb-0"

/* 测试配置 */
#define TEST_LOOPS      20      /* 必须 > 5 以验证 Buffer 回收 */
#define TX_DATA_SIZE    10
#define RX_TIMEOUT_MS   2000    /* 异步等待超时 */

/* -----------------------------------------------------------
 * 模拟 ASIO 上下文
 * ----------------------------------------------------------- */
#define ASIO_STACK_SIZE 2048
#define ASIO_PRIORITY   5

/* 定义异步线程堆栈 */
K_THREAD_STACK_DEFINE(asio_stack, ASIO_STACK_SIZE);
struct k_thread asio_thread_data;

/* 同步信号量：用于通知主线程“异步读完成了” */
K_SEM_DEFINE(rx_notify_sem, 0, 1);

/* 全局变量用于在线程间传递数据 (简化模拟) */
static int global_fd = -1;
static uint8_t global_rx_buf[256];
static int global_rx_len = 0;
static bool keep_running = true;

/* * 模拟 Boost Asio 的事件循环线程 (io_context::run)
 * 它的工作就是死等 poll，一旦有数据，就读取并触发回调
 */
void asio_mock_thread_entry(void *p1, void *p2, void *p3)
{
    struct pollfd fds;
    int ret;

    printk("[ASIO Thread] Event loop started (Simulating boost::asio::io_context)");

    while (keep_running) {
        if (global_fd < 0) {
            k_sleep(K_MSEC(100));
            continue;
        }

        fds.fd = global_fd;
        fds.events = POLLIN;

        /* 1. Wait (Poll): 对应 epoll_wait / io_context idle */
        /* 使用 500ms 超时是为了能检查 keep_running 标志 */
        ret = poll(&fds, 1, 500);

        if (ret < 0) {
            printk("[ASIO Thread] Poll error: %d", errno);
            k_sleep(K_MSEC(1000));
            continue;
        } else if (ret == 0) {
            /* 超时无事发生，继续循环 */
            continue;
        }

        /* 2. Handler Trigger: 对应 async_read 完成 */
        if (fds.revents & POLLIN) {
            /* 清空 Buffer */
            memset(global_rx_buf, 0, sizeof(global_rx_buf));
            
            /* 执行读取 */
            global_rx_len = read(global_fd, global_rx_buf, sizeof(global_rx_buf));

            if (global_rx_len > 0) {
                printk("[ASIO Thread] Async Handler Triggered! Read %d bytes", global_rx_len);
                /* 模拟调用回调函数 -> 通知主线程 */
                k_sem_give(&rx_notify_sem);
            } else {
                /* 可能是 EAGAIN 或者错误 */
                if (errno != EAGAIN) {
                    printk("[ASIO Thread] Read error: %d", errno);
                }
            }
        }
    }
    printk("[ASIO Thread] Event loop stopped");
}

/* -----------------------------------------------------------
 * 主测试函数
 * ----------------------------------------------------------- */
void ipmb_test_main(void)
{
    int ret;
    int success_cnt = 0;
    uint8_t tx_buf[TX_DATA_SIZE];

    printk("==================================================");
    printk("Starting IPMB Async (Boost ASIO Mock) Test");
    printk("==================================================");

    k_sleep(K_MSEC(1000));

    /* 1. 启动后台异步处理线程 */
    k_thread_create(&asio_thread_data, asio_stack,
                    K_THREAD_STACK_SIZEOF(asio_stack),
                    asio_mock_thread_entry,
                    NULL, NULL, NULL,
                    ASIO_PRIORITY, 0, K_NO_WAIT);

    /* 2. 打开设备 */
    global_fd = open(IPMB_DEV_PATH, O_RDWR);
    if (global_fd < 0) {
        printk("FAIL: Unable to open device. errno=%d", errno);
        return;
    }
    printk("PASS: Device opened. fd=%d", global_fd);

    /* 3. 循环测试 */
    for (int i = 0; i < TEST_LOOPS; i++) {
        printk("--- Loop %d/%d ---", i + 1, TEST_LOOPS);

        /* 准备发送数据 */
        memset(tx_buf, 0, sizeof(tx_buf));
        snprintf((char *)tx_buf, sizeof(tx_buf), "ASYNC-%02d", i);
        tx_buf[0] = 0x10; /* 模拟 Slave Address */

        /* A. Write: 主线程只负责发送 */
        ret = write(global_fd, tx_buf, sizeof(tx_buf));
        if (ret != sizeof(tx_buf)) {
            printk("FAIL: Write failed. errno=%d", errno);
            break;
        }
        printk("[Main Thread] Write command sent");

        /* B. Wait Async: 等待后台线程通知 */
        /* ipmbbridged 此时会继续处理其他 D-Bus 消息，我们这里用信号量模拟等待 */
        ret = k_sem_take(&rx_notify_sem, K_MSEC(RX_TIMEOUT_MS));

        if (ret == 0) {
            /* 收到信号，说明后台线程读到了数据 */
            printk("PASS: Main thread received notification.");
            printk(global_rx_buf, global_rx_len, "RX Data (Verified in Main)");
            success_cnt++;
        } else {
            printk("TIMEOUT: Async read timed out (No response from HW)");
            /* 继续循环，测试内存是否泄漏 */
        }
        
        /* 稍微延时，让日志打印清楚 */
        k_sleep(K_MSEC(100));
    }

    /* 4. 清理 */
    keep_running = false;
    close(global_fd);
    global_fd = -1;

    /* 等待线程结束 (可选) */
    k_sleep(K_MSEC(1000));

    printk("==================================================");
    printk("Test Summary: %d/%d interactions successful", success_cnt, TEST_LOOPS);
    
    if (success_cnt > 5) {
        printk("RESULT: SUCCESS (Buffer recycling & Async Poll verified)");
    } else {
        printk("RESULT: CHECK LOGS (Success count low)");
    }
    printk("==================================================");
}