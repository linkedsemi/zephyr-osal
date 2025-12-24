
#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/poll.h>
#include <errno.h>


#define IPMB_DEV_PATH   "/dev/ipmb/ipmb-0"
#define TEST_LOOPS      20
#define TX_DATA_SIZE    16
#define RX_BUF_SIZE     256
#define RX_TIMEOUT_MS   2000

/* -----------------------------------------------------------
 * 模拟 ASIO 线程上下文
 * ----------------------------------------------------------- */
#define ASIO_STACK_SIZE 2048
#define ASIO_PRIORITY   5

K_THREAD_STACK_DEFINE(asio_stack, ASIO_STACK_SIZE);
struct k_thread asio_thread_data;
K_SEM_DEFINE(rx_notify_sem, 0, 1);

static int global_fd = -1;
static uint8_t global_rx_buf[RX_BUF_SIZE];
static int global_rx_len = 0;
static bool keep_running = true;

/* 控制 ASIO 线程是否读取数据 (用于测试 Buffer 溢出) */
static atomic_t asio_pause_read = ATOMIC_INIT(0);

static void print_hex(const char *desc, const uint8_t *data, size_t len)
{
    printk("%s: ", desc);
    for (size_t i = 0; i < len; i++) {
        printk("%02X ", data[i]);
    }
    printk("\n");
}

/* * 模拟 Boost Asio 线程
 */
void asio_mock_thread_entry(void *p1, void *p2, void *p3)
{
    struct pollfd fds;
    int ret;

    printk("[ASIO Thread] Started.\n");

    while (keep_running) {
        if (global_fd < 0) {
            k_sleep(K_MSEC(100));
            continue;
        }

        /* 如果处于“暂停读取”模式（测试溢出），则只 Sleep 不 Poll */
        if (atomic_get(&asio_pause_read)) {
            k_sleep(K_MSEC(100));
            continue;
        }

        fds.fd = global_fd;
        fds.events = POLLIN;

        /* 1. Wait (Poll) */
        ret = poll(&fds, 1, 500);

        if (ret < 0) {
            printk("[ASIO] Poll error: %d\n", errno);
            k_sleep(K_MSEC(1000));
            continue;
        } else if (ret == 0) {
            continue;
        }

        /* 2. Handler Trigger */
        if (fds.revents & POLLIN) {
            memset(global_rx_buf, 0, sizeof(global_rx_buf));
            
            /* 执行 Read */
            global_rx_len = read(global_fd, global_rx_buf, sizeof(global_rx_buf));

            if (global_rx_len > 0) {
                // printk("[ASIO] Async Read %d bytes\n", global_rx_len);
                k_sem_give(&rx_notify_sem);
            } else {
                if (errno != EAGAIN) {
                    printk("[ASIO] Read error: %d\n", errno);
                }
            }
        }
    }
    printk("[ASIO Thread] Stopped.\n");
}

/* -----------------------------------------------------------
 * 辅助校验函数
 * ----------------------------------------------------------- */
static int verify_data(uint8_t *tx, int tx_len, uint8_t *rx, int rx_len)
{
    /* 1. 检查总长度 (Loopback 驱动会增加 1 字节长度头) */
    int expected_len = tx_len + 1;
    if (rx_len != expected_len) {
        printk("FAIL: Len mismatch! Expect %d, Got %d\n", expected_len, rx_len);
        return -1;
    }

    /* 2. 检查 Header Byte (它是 Payload 长度) */
    if (rx[0] != (uint8_t)tx_len) {
        printk("FAIL: Header mismatch! Expect %02X, Got %02X\n", tx_len, rx[0]);
        return -1;
    }

    /* 3. 检查 Payload 内容 */
    if (memcmp(tx, &rx[1], tx_len) != 0) {
        printk("FAIL: Payload mismatch!\n");
        print_hex("Sent", tx, tx_len);
        print_hex("Recv", &rx[1], tx_len);
        return -1;
    }

    return 0;
}

/* -----------------------------------------------------------
 * 测试用例集合
 * ----------------------------------------------------------- */

/* CASE 1: 标准读写循环 + 数据校验 */
void test_standard_loopback(void)
{
    printk("=== TEST 1: Standard Loopback & Verify (%d Loops) ===\n", TEST_LOOPS);
    int ret, success_cnt = 0;
    uint8_t tx_buf[TX_DATA_SIZE];

    for (int i = 0; i < TEST_LOOPS; i++) {
        /* 构造带序号的数据 */
        memset(tx_buf, 0, sizeof(tx_buf));
        snprintf((char *)tx_buf, sizeof(tx_buf), "MSG-%04d", i);
        
        /* Write */
        ret = write(global_fd, tx_buf, sizeof(tx_buf));
        if (ret != sizeof(tx_buf)) {
            printk("Write failed loop %d\n", i);
            break;
        }

        /* Async Wait */
        if (k_sem_take(&rx_notify_sem, K_MSEC(RX_TIMEOUT_MS)) == 0) {
            /* Verify */
            if (verify_data(tx_buf, sizeof(tx_buf), global_rx_buf, global_rx_len) == 0) {
                success_cnt++;
            }
        } else {
            printk("Timeout at loop %d\n", i);
        }
        k_sleep(K_MSEC(20));
    }
    
    if (success_cnt == TEST_LOOPS) {
        printk("RESULT: PASS\n");
    } else {
        printk("RESULT: FAIL (%d/%d)\n", success_cnt, TEST_LOOPS);
    }
}

/* CASE 2: LSEEK 兼容性测试 */
void test_lseek(void)
{
    printk("=== TEST 2: Lseek Compatibility ===\n");
    
    /* 对流设备 seek，应该返回 0 或成功，不应崩溃 */
    off_t pos = lseek(global_fd, 0, SEEK_CUR);
    if (pos == 0) {
        printk("lseek(SEEK_CUR) returned 0 (Expected for stream dev)\n");
    } else if (pos < 0) {
        printk("lseek failed errno=%d (Check if ENOTSUP is handled)\n", errno);
    }

    pos = lseek(global_fd, 100, SEEK_SET);
    if (pos == 0) {
        printk("lseek(SEEK_SET) returned 0 (Ignored as expected)\n");
    }
    printk("RESULT: PASS (If no crash occurred)\n");
}

/* CASE 3: Buffer 溢出测试 */
void test_buffer_overflow(void)
{
    printk("=== TEST 3: Buffer Overflow / Starvation ===\n");
    /* 假设驱动有 5 个 Buffer。我们发送 7 个，暂停读取，看是否丢包且不崩 */
    
    uint8_t tx_buf[10] = {0xAA, 0xBB};
    int sent_count = 8; /* 发送数量 > RX_BUF_COUNT (5) */

    /* 1. 暂停 ASIO 线程读取，让驱动层积压数据 */
    printk("Pausing ASIO Read thread...\n");
    atomic_set(&asio_pause_read, 1);
    k_sleep(K_MSEC(200)); 

    /* 2. 连续猛烈写入 */
    for (int i = 0; i < sent_count; i++) {
        tx_buf[1] = i; // 标记序号
        int ret = write(global_fd, tx_buf, sizeof(tx_buf));
        if (ret > 0) {
            printk("Blast Write %d OK\n", i);
        } else {
            printk("Blast Write %d Failed\n", i);
        }
        /* 极短延时，模拟爆发流量 */
        k_busy_wait(100); 
    }

    printk("Check logs above: You should see 'Drop: No buffers' for later packets\n");

    /* 3. 恢复读取，看能不能把剩下的读出来 */
    printk("Resuming ASIO Read thread...\n");
    atomic_set(&asio_pause_read, 0);

    /* 4. 等待队列排空 */
    int drained_cnt = 0;
    while(k_sem_take(&rx_notify_sem, K_MSEC(500)) == 0) {
        drained_cnt++;
        printk("Drained packet: Header=%d, Data[1]=%d\n", global_rx_buf[0], global_rx_buf[2]);
    }

    printk("Drained %d packets total.\n", drained_cnt);
    /* 驱动里只有 5 个 Buffer，所以理论上只能读出来 5 个，其他的应该被丢弃了 */
    if (drained_cnt <= 5 && drained_cnt > 0) {
        printk("RESULT: PASS (Overflow handled gracefully)\n");
    } else {
        printk("RESULT: UNCERTAIN (Drained %d, expected <= 5)\n", drained_cnt);
    }
}

/* CASE 4: 边界数据测试 */
void test_edge_cases(void)
{
    printk("=== TEST 4: Edge Cases (Zero/Large Length) ===\n");
    
    /* A. 超长数据 (大于 240) */
    uint8_t huge_buf[300];
    memset(huge_buf, 0xEE, sizeof(huge_buf));
    int ret = write(global_fd, huge_buf, sizeof(huge_buf));
    
    /* 你的驱动目前截断到 239，返回 239 或 300 都是合理的，只要不崩 */
    printk("Write 300 bytes returned: %d\n", ret);
    
    if (k_sem_take(&rx_notify_sem, K_MSEC(1000)) == 0) {
        printk("Read back huge packet len: %d (First byte=%02X)\n", global_rx_len, global_rx_buf[0]);
    }

    /* B. 极短数据 */
    uint8_t tiny_buf[1] = {0xFF};
    write(global_fd, tiny_buf, 1);
    k_sem_take(&rx_notify_sem, K_MSEC(1000));
    
    printk("RESULT: PASS (System survived edge inputs)\n");
}

/* -----------------------------------------------------------
 * Main Entry
 * ----------------------------------------------------------- */
void ipmb_test_main(void)
{
    k_sleep(K_MSEC(2000)); /* 等待系统完全启动 */
    
    printk("======================================\n");
    printk("  IPMB FULL SUITE INTEGRATION TEST    \n");
    printk("======================================\n");

    /* 1. 启动线程 */
    k_thread_create(&asio_thread_data, asio_stack,
                    K_THREAD_STACK_SIZEOF(asio_stack),
                    asio_mock_thread_entry,
                    NULL, NULL, NULL,
                    ASIO_PRIORITY, 0, K_NO_WAIT);

    /* 2. 打开设备 */
    global_fd = open(IPMB_DEV_PATH, O_RDWR);
    if (global_fd < 0) {
        printk("FATAL: Open failed %d\n", errno);
        return;
    }

    /* 3. 执行测试套件 */
    test_lseek();               /* 测试 seek */
    k_sleep(K_MSEC(500));
    
    test_standard_loopback();   /* 测试标准读写和校验 */
    k_sleep(K_MSEC(500));
    
    test_edge_cases();          /* 测试超长/超短 */
    k_sleep(K_MSEC(500));

    test_buffer_overflow();     /* 测试丢包稳定性 */

    /* 4. 清理 */
    keep_running = false;
    close(global_fd);
    printk("======================================\n");
    printk("  ALL TESTS COMPLETED                 \n");
    printk("======================================\n");
}
