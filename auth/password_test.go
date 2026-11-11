package main

import (
	"errors"
	"strings"
	"testing"
)

func TestValidatePassword(t *testing.T) {
	cases := []struct {
		pw   string
		want error
	}{
		{"", ErrPasswordTooShort},
		{"1234567", ErrPasswordTooShort},
		{"12345678", ErrPasswordAllDigits},
		{"12345678901234567890", ErrPasswordAllDigits},
		{"abc12345", nil},
		{"密码密码密码密码", nil},              // 8 个中文字符，按 rune 计数应通过
		{strings.Repeat("a", 73), ErrPasswordTooLong}, // 超 72 字节必须拒绝（bcrypt 会静默截断）
	}
	for _, c := range cases {
		got := validatePassword(c.pw)
		if !errors.Is(got, c.want) {
			t.Errorf("validatePassword(%q) = %v, 期望 %v", c.pw, got, c.want)
		}
	}
}

func TestHashAndVerify(t *testing.T) {
	const pw = "correct horse battery staple"
	hash, err := hashPassword(pw)
	if err != nil {
		t.Fatalf("哈希失败: %v", err)
	}

	// 必须是 bcrypt 格式，绝不能是明文或裸 sha
	if !strings.HasPrefix(hash, "$2a$12$") && !strings.HasPrefix(hash, "$2b$12$") && !strings.HasPrefix(hash, "$2y$12$") {
		t.Fatalf("哈希格式不对（cost 应为 12）: %s", hash)
	}
	if strings.Contains(hash, pw) {
		t.Fatal("哈希里不应出现明文")
	}

	if !verifyPassword(hash, pw) {
		t.Fatal("正确密码应校验通过")
	}
	if verifyPassword(hash, pw+"x") {
		t.Fatal("错误密码不应通过")
	}
}

// 同一个密码两次哈希结果必须不同（bcrypt 自带随机 salt）
func TestHashIsSalted(t *testing.T) {
	a, err1 := hashPassword("same-password")
	b, err2 := hashPassword("same-password")
	if err1 != nil || err2 != nil {
		t.Fatalf("哈希失败: %v %v", err1, err2)
	}
	if a == b {
		t.Fatal("两次哈希结果相同，说明没有随机 salt")
	}
	if !verifyPassword(a, "same-password") || !verifyPassword(b, "same-password") {
		t.Fatal("两个哈希都应能校验通过")
	}
}

// 未初始化 dummyHash 时 verifyAgainstDummy 不能 panic
func TestVerifyAgainstDummyWithoutInit(t *testing.T) {
	saved := dummyHash
	dummyHash = nil
	defer func() { dummyHash = saved }()
	verifyAgainstDummy("anything")
}
