package main

import (
	"sync"
	"time"
)

// Limiter 是按键（IP / 用户名）的令牌桶限流器。
//
// 为什么用令牌桶而不是"固定窗口计数"：
//   固定窗口在窗口边界会被打双倍（00:59 打满 + 01:00 再打满）；
//   令牌桶允许短时突发（burst），但长期速率被 rate 限制，更贴近真实登录行为。
type Limiter struct {
	mu      sync.Mutex
	buckets map[string]*bucket
	rate    float64 // 每秒补充的令牌数
	burst   float64 // 桶容量（允许的瞬时突发次数）
}

type bucket struct {
	tokens float64
	last   time.Time
}

func NewLimiter(perMinute, burst int) *Limiter {
	return &Limiter{
		buckets: make(map[string]*bucket),
		rate:    float64(perMinute) / 60.0,
		burst:   float64(burst),
	}
}

// Allow 判断该 key 当前是否允许通过。
func (l *Limiter) Allow(key string) bool {
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()

	b, ok := l.buckets[key]
	if !ok {
		l.buckets[key] = &bucket{tokens: l.burst - 1, last: now}
		return true
	}

	// 按流逝时间补充令牌
	elapsed := now.Sub(b.last).Seconds()
	if elapsed > 0 {
		b.tokens += elapsed * l.rate
		if b.tokens > l.burst {
			b.tokens = l.burst
		}
		b.last = now
	}

	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// GC 清理长时间不活跃的 key，避免被大量随机 IP 撑爆内存。
// 由 main 里的后台协程定期调用。
func (l *Limiter) GC(olderThan time.Duration) int {
	deadline := time.Now().Add(-olderThan)
	l.mu.Lock()
	defer l.mu.Unlock()
	removed := 0
	for k, b := range l.buckets {
		if b.last.Before(deadline) {
			delete(l.buckets, k)
			removed++
		}
	}
	return removed
}

func (l *Limiter) Size() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.buckets)
}
