package main

import (
	"errors"
	"unicode/utf8"

	"golang.org/x/crypto/bcrypt"
)

// bcryptCost=12 在 2 核机器上单次校验约 200~400ms。
// 这是刻意的：让离线爆破的代价足够高，同时登录接口本身不频繁。
const bcryptCost = 12

var (
	ErrPasswordTooShort  = errors.New("密码至少 8 个字符")
	ErrPasswordTooLong   = errors.New("密码过长（bcrypt 上限 72 字节）")
	ErrPasswordAllDigits = errors.New("密码不能是纯数字")
)

// dummyHash 用于"用户不存在"时也执行一次等量 bcrypt 校验。
// 否则"用户不存在"会立刻返回、"密码错误"要算 300ms，
// 攻击者靠响应时间就能枚举出哪些用户名已注册。
//
// 由 main 在启动时通过 initDummyHash 填充（生成一次约 300ms，可接受）。
var dummyHash []byte

func initDummyHash() error {
	h, err := bcrypt.GenerateFromPassword([]byte("timing-equalizer-not-a-real-password"), bcryptCost)
	if err != nil {
		return err
	}
	dummyHash = h
	return nil
}

// validatePassword 做最基本的强度校验。
// 真实项目里还会查"常见弱密码字典"，这里保持简单。
func validatePassword(pw string) error {
	if utf8.RuneCountInString(pw) < 8 {
		return ErrPasswordTooShort
	}
	// bcrypt 只处理前 72 字节，超长会静默截断——必须显式拒绝，否则输入被悄悄丢弃
	if len(pw) > 72 {
		return ErrPasswordTooLong
	}
	allDigits := true
	for _, r := range pw {
		if r < '0' || r > '9' {
			allDigits = false
			break
		}
	}
	if allDigits {
		return ErrPasswordAllDigits
	}
	return nil
}

func hashPassword(pw string) (string, error) {
	h, err := bcrypt.GenerateFromPassword([]byte(pw), bcryptCost)
	if err != nil {
		return "", err
	}
	return string(h), nil
}

func verifyPassword(hash, pw string) bool {
	return bcrypt.CompareHashAndPassword([]byte(hash), []byte(pw)) == nil
}

// verifyAgainstDummy 消耗与真实校验相当的时间，用于抹平时序差异。
func verifyAgainstDummy(pw string) {
	if len(dummyHash) == 0 {
		return
	}
	_ = bcrypt.CompareHashAndPassword(dummyHash, []byte(pw))
}
