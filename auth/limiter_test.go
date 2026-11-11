package main

import (
	"testing"
	"time"
)

func TestLimiterBurstThenDeny(t *testing.T) {
	// 每分钟 60 次、突发 3：允许连续 3 次，第 4 次立即被拒
	l := NewLimiter(60, 3)
	for i := 0; i < 3; i++ {
		if !l.Allow("ip1") {
			t.Fatalf("第 %d 次应放行", i+1)
		}
	}
	if l.Allow("ip1") {
		t.Fatal("第 4 次应被拒")
	}
}

func TestLimiterIsolatesKeys(t *testing.T) {
	l := NewLimiter(60, 1)
	if !l.Allow("a") {
		t.Fatal("a 第一次应放行")
	}
	if l.Allow("a") {
		t.Fatal("a 第二次应被拒")
	}
	if !l.Allow("b") {
		t.Fatal("不同 key 不应相互影响")
	}
}

func TestLimiterRefill(t *testing.T) {
	// 每秒 1 个令牌，突发 1
	l := NewLimiter(60, 1)
	if !l.Allow("k") {
		t.Fatal("第一次应放行")
	}
	if l.Allow("k") {
		t.Fatal("第二次应被拒")
	}

	// 手动把时间戳往前拨 2 秒，模拟令牌补充（不依赖 sleep）
	l.mu.Lock()
	if b, ok := l.buckets["k"]; ok {
		b.last = b.last.Add(-2 * time.Second)
	}
	l.mu.Unlock()

	if !l.Allow("k") {
		t.Fatal("补充令牌后应放行")
	}
}

func TestLimiterGC(t *testing.T) {
	l := NewLimiter(60, 1)
	l.Allow("old")
	l.Allow("new")

	l.mu.Lock()
	l.buckets["old"].last = time.Now().Add(-2 * time.Hour)
	l.mu.Unlock()

	if n := l.GC(time.Hour); n != 1 {
		t.Fatalf("应清理 1 个冷 key，实际 %d", n)
	}
	if l.Size() != 1 {
		t.Fatalf("剩余应为 1，实际 %d", l.Size())
	}
}
