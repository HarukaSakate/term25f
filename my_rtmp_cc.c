// my_rtmp_cc.c - 動的待機時間版
#include "vmlinux.h"
#include <bpf_helpers.h>
#include <bpf_core_read.h>
#include <bpf_endian.h>
#include <bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

// 待機時間の範囲定義（ナノ秒）
#define MIN_DELAY_NS (500000000ULL)     // 最小 0.5秒
#define MAX_DELAY_NS (10000000000ULL)   // 最大 10秒
#define DEFAULT_DELAY_NS (3000000000ULL) // デフォルト 3秒

// RTT閾値（ナノ秒換算: 50ms, 100ms, 200ms）
#define RTT_EXCELLENT (50000000ULL)   // 50ms以下: 優秀
#define RTT_GOOD      (100000000ULL)  // 100ms以下: 良好
#define RTT_POOR      (200000000ULL)  // 200ms以下: 不良

// パケットロス率の閾値（パーミル: 千分率）
#define LOSS_RATE_LOW    10   // 1%以下
#define LOSS_RATE_MEDIUM 30   // 3%以下
#define LOSS_RATE_HIGH   50   // 5%以下

// 統計情報を保持する構造体
struct cc_stats {
    __u64 total_packets;      // 総送信パケット数
    __u64 lost_packets;       // 総ロストパケット数
    __u64 last_rtt;           // 最新のRTT（マイクロ秒）
    __u32 consecutive_losses; // 連続ロス回数
    __u64 last_loss_time;     // 最後のロス発生時刻
    __u64 dynamic_delay;      // 計算された動的待機時間
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, __u64);  // 輻輳開始時刻(ns)
} congestion_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, bool);
} congestion_flag_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, __u32);
} window_size_map SEC(".maps");

// 統計情報マップ
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct cc_stats);
} stats_map SEC(".maps");

static __always_inline struct tcp_sock *tcp_sk(const struct sock *sk)
{
    return (struct tcp_sock *)sk;
}

// 動的待機時間を計算する関数
static __always_inline __u64 calculate_dynamic_delay(struct cc_stats *stats, __u64 now)
{
    __u64 delay = DEFAULT_DELAY_NS;
    __u64 rtt_ns = stats->last_rtt * 1000; // マイクロ秒からナノ秒に変換
    
    // パケットロス率を計算（パーミル: 千分率）
    __u32 loss_rate = 0;
    if (stats->total_packets > 0) {
        loss_rate = (stats->lost_packets * 1000) / stats->total_packets;
    }
    
    // === 1. RTTベースの調整 ===
    // RTTが低い = ネットワーク状態良好 → 待機時間を短く
    if (rtt_ns < RTT_EXCELLENT) {
        delay = delay * 60 / 100;  // -40%
        bpf_printk("RTT excellent (%llu ns), reducing delay by 40%%", rtt_ns);
    } else if (rtt_ns < RTT_GOOD) {
        delay = delay * 80 / 100;  // -20%
        bpf_printk("RTT good (%llu ns), reducing delay by 20%%", rtt_ns);
    } else if (rtt_ns > RTT_POOR) {
        delay = delay * 140 / 100; // +40%
        bpf_printk("RTT poor (%llu ns), increasing delay by 40%%", rtt_ns);
    }
    
    // === 2. パケットロス率ベースの調整 ===
    if (loss_rate < LOSS_RATE_LOW) {
        // ロス率が低い → 待機時間を長く（積極的に帯域を維持）
        delay = delay * 130 / 100; // +30%
        bpf_printk("Loss rate low (%u‰), increasing delay by 30%%", loss_rate);
    } else if (loss_rate > LOSS_RATE_HIGH) {
        // ロス率が高い → 待機時間を短く（早めに帯域を減らす）
        delay = delay * 70 / 100;  // -30%
        bpf_printk("Loss rate high (%u‰), reducing delay by 30%%", loss_rate);
    } else if (loss_rate > LOSS_RATE_MEDIUM) {
        delay = delay * 85 / 100;  // -15%
        bpf_printk("Loss rate medium (%u‰), reducing delay by 15%%", loss_rate);
    }
    
    // === 3. 連続ロス回数ベースの調整 ===
    if (stats->consecutive_losses > 5) {
        // 連続でロスが発生している → 深刻な輻輳、早めに対応
        delay = delay * 50 / 100;  // -50%
        bpf_printk("Consecutive losses (%u), halving delay", stats->consecutive_losses);
    } else if (stats->consecutive_losses > 3) {
        delay = delay * 70 / 100;  // -30%
        bpf_printk("Moderate consecutive losses (%u), reducing delay by 30%%", 
                   stats->consecutive_losses);
    }
    
    // === 4. ロス発生頻度ベースの調整 ===
    if (stats->last_loss_time > 0) {
        __u64 time_since_last_loss = now - stats->last_loss_time;
        // 最後のロスから1秒以内に再度ロス発生 → 頻繁なロス
        if (time_since_last_loss < 1000000000ULL) {
            delay = delay * 60 / 100;  // -40%
            bpf_printk("Frequent packet loss detected, reducing delay by 40%%");
        }
        // 最後のロスから5秒以上経過 → ネットワーク回復傾向
        else if (time_since_last_loss > 5000000000ULL) {
            delay = delay * 120 / 100; // +20%
            bpf_printk("Network recovering, increasing delay by 20%%");
        }
    }
    
    // 最小値・最大値でクリップ
    if (delay < MIN_DELAY_NS) {
        delay = MIN_DELAY_NS;
        bpf_printk("Delay clamped to minimum: %llu ns", delay);
    }
    if (delay > MAX_DELAY_NS) {
        delay = MAX_DELAY_NS;
        bpf_printk("Delay clamped to maximum: %llu ns", delay);
    }
    
    // 秒単位に変換（整数演算のみ）
    __u64 delay_sec = delay / 1000000000ULL;
    __u64 delay_msec = (delay % 1000000000ULL) / 1000000ULL;
    bpf_printk("Calculated dynamic delay: %llu ns (%llu.%03llu sec)", 
               delay, delay_sec, delay_msec);
    
    return delay;
}

// cong_ops: ssthresh計算
SEC("struct_ops/my_rtmp_cc_ssthresh")
__u32 my_rtmp_cc_ssthresh(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    __u32 cwnd = BPF_CORE_READ(tp, snd_cwnd);
    return cwnd / 2 < 2 ? 2 : cwnd / 2;
}

extern void tcp_reno_cong_avoid(struct sock *sk, u32 ack, u32 acked) __ksym;

SEC("struct_ops")
void BPF_PROG(my_rtmp_cc_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
    struct tcp_sock *tp = tcp_sk(sk);
    
    __u16 num = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u64 sid = (__u64)num;

    __u64 *start_time = bpf_map_lookup_elem(&congestion_start_map, &sid);
    bool *in_cong = bpf_map_lookup_elem(&congestion_flag_map, &sid);
    struct cc_stats *stats = bpf_map_lookup_elem(&stats_map, &sid);

    if (!in_cong || !start_time || !stats) {
        __u64 zero = 0;
        bool false_val = false;
        struct cc_stats init_stats = {0};
        init_stats.dynamic_delay = DEFAULT_DELAY_NS;
        
        bpf_map_update_elem(&congestion_start_map, &sid, &zero, BPF_ANY);
        bpf_map_update_elem(&congestion_flag_map, &sid, &false_val, BPF_ANY);
        bpf_map_update_elem(&stats_map, &sid, &init_stats, BPF_ANY);
        return;
    }

    __u32 lost_out = BPF_CORE_READ(tp, lost_out);
    __u64 now = bpf_ktime_get_ns();
    
    // RTT情報を更新（マイクロ秒）
    __u32 srtt = BPF_CORE_READ(tp, srtt_us);
    stats->last_rtt = srtt >> 3; // srtt_usは8倍されているので元に戻す
    
    // 総パケット数を更新
    stats->total_packets = BPF_CORE_READ(tp, segs_out);

    if (lost_out > 0) {
        // パケットロス検出
        *in_cong = true;
        
        // 統計情報を更新
        __u32 prev_lost = stats->lost_packets;
        stats->lost_packets = BPF_CORE_READ(tp, lost_out);
        
        // 新規ロスの場合
        if (stats->lost_packets > prev_lost) {
            // 連続ロスカウントを更新
            if (stats->last_loss_time > 0 && 
                (now - stats->last_loss_time) < 1000000000ULL) {
                stats->consecutive_losses++;
            } else {
                stats->consecutive_losses = 1;
            }
            stats->last_loss_time = now;
        }
        
        if (*start_time == 0) {
            *start_time = now;
            // 動的待機時間を計算
            stats->dynamic_delay = calculate_dynamic_delay(stats, now);
            bpf_printk("Congestion detected, dynamic delay: %llu ns", 
                       stats->dynamic_delay);
        }
    } else {
        // パケットロスなし
        if (*in_cong) {
            // 輻輳から回復
            stats->consecutive_losses = 0;
        }
        *in_cong = false;
        *start_time = 0;
    }

    __u32 cwnd_before = BPF_CORE_READ(tp, snd_cwnd);

    // 動的待機時間を使用した輻輳制御
    if (*in_cong) {
        __u64 elapsed = now - *start_time;
        if (elapsed >= stats->dynamic_delay) {
            tcp_reno_cong_avoid(sk, ack, acked);
            bpf_printk("Dynamic delay elapsed (%llu ns), adjusting cwnd", elapsed);
        } else {
            bpf_printk("Waiting... elapsed: %llu ns, delay: %llu ns", 
                       elapsed, stats->dynamic_delay);
        }
    } else {
        tcp_reno_cong_avoid(sk, ack, acked);
    }

    // ウィンドウサイズ変更検出
    __u32 cwnd_after = BPF_CORE_READ(tp, snd_cwnd);
    __u32 *prev_cwnd_p = bpf_map_lookup_elem(&window_size_map, &sid);
    
    if (!prev_cwnd_p) {
        bpf_map_update_elem(&window_size_map, &sid, &cwnd_after, BPF_ANY);
    } else {
        if (*prev_cwnd_p != cwnd_after) {
            bpf_printk("Window size changed: %u -> %u (RTT: %llu us, Loss rate: %u‰)", 
                       *prev_cwnd_p, cwnd_after, stats->last_rtt,
                       stats->total_packets > 0 ? 
                       (stats->lost_packets * 1000) / stats->total_packets : 0);
            bpf_map_update_elem(&window_size_map, &sid, &cwnd_after, BPF_ANY);
        }
    }
}

// cong_ops: undo_cwnd
SEC("struct_ops/my_rtmp_cc_undo_cwnd")
__u32 my_rtmp_cc_undo_cwnd(struct sock *sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    __u32 cwnd = BPF_CORE_READ(tp, snd_cwnd);
    return cwnd < 10 ? 10 : cwnd;
}

// init
SEC("struct_ops/my_rtmp_cc_init")
void my_rtmp_cc_init(struct sock *sk) {
    __u16 num = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u64 sid = (__u64)num;
    __u64 zero = 0;
    bool false_val = false;
    
    struct cc_stats init_stats = {0};
    init_stats.dynamic_delay = DEFAULT_DELAY_NS;
    
    bpf_map_update_elem(&congestion_start_map, &sid, &zero, BPF_ANY);
    bpf_map_update_elem(&congestion_flag_map, &sid, &false_val, BPF_ANY);
    bpf_map_update_elem(&stats_map, &sid, &init_stats, BPF_ANY);

    struct tcp_sock *tp = tcp_sk(sk);
    __u32 init_cwnd = BPF_CORE_READ(tp, snd_cwnd);
    bpf_map_update_elem(&window_size_map, &sid, &init_cwnd, BPF_ANY);

    bpf_printk("Socket initialized with default delay: %llu ns", DEFAULT_DELAY_NS);
}

// release
SEC("struct_ops/my_rtmp_cc_release")
void my_rtmp_cc_release(struct sock *sk) {
    __u16 num = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u64 sid = (__u64)num;
    bpf_map_delete_elem(&congestion_start_map, &sid);
    bpf_map_delete_elem(&congestion_flag_map, &sid);
    bpf_map_delete_elem(&window_size_map, &sid);
    bpf_map_delete_elem(&stats_map, &sid);
}

SEC(".struct_ops") 
struct tcp_congestion_ops my_rtmp_cc_ops = {
    .init = (void *)my_rtmp_cc_init,
    .release = (void *)my_rtmp_cc_release,
    .ssthresh = (void *)my_rtmp_cc_ssthresh,
    .cong_avoid = (void *)my_rtmp_cc_cong_avoid,
    .undo_cwnd = (void *)my_rtmp_cc_undo_cwnd,
    .name       = "my_rtmp_cc",
};
