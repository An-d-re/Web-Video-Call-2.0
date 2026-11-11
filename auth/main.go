package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	cfg, err := LoadConfig()
	if err != nil {
		log.Fatalf("配置错误: %v", err)
	}

	// 时序均衡哈希：登录时"用户不存在"也要算一次 bcrypt，启动时预生成（约 300ms）
	if err := initDummyHash(); err != nil {
		log.Fatalf("初始化失败: %v", err)
	}

	// Ctrl+C / SIGTERM 时取消，触发优雅退出
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	pool, err := NewPool(ctx, cfg.DatabaseURL)
	if err != nil {
		log.Fatalf("%v", err)
	}
	defer pool.Close()

	srv := NewServer(cfg, pool)

	// 后台维护：清理过期令牌 + 回收限流器里的冷 key
	go func() {
		ticker := time.NewTicker(6 * time.Hour)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if n, err := srv.store.PurgeExpiredTokens(ctx); err != nil {
					log.Printf("清理过期令牌失败: %v", err)
				} else if n > 0 {
					log.Printf("清理过期令牌 %d 条", n)
				}
				srv.loginIP.GC(time.Hour)
				srv.loginByUser.GC(time.Hour)
				srv.registerIP.GC(time.Hour)
			}
		}
	}()

	httpSrv := &http.Server{
		Addr:              cfg.ListenAddr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	go func() {
		<-ctx.Done()
		log.Printf("收到退出信号，开始优雅退出…")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := httpSrv.Shutdown(shutdownCtx); err != nil {
			log.Printf("优雅退出失败: %v", err)
		}
	}()

	log.Printf("auth 服务启动，监听 %s", cfg.ListenAddr)
	if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("HTTP 服务异常退出: %v", err)
	}
	log.Printf("auth 服务已停止")
}
