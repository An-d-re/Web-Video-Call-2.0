module webrtc/auth

go 1.26.0

// 依赖不在 go.mod 里预置版本，避免猜错版本号。
// 首次构建请执行：
//   go mod tidy
// 国内网络建议：
//   go env -w GOPROXY=https://goproxy.cn,direct

require (
	github.com/jackc/pgx/v5 v5.12.0
	golang.org/x/crypto v0.58.0
)

require (
	github.com/jackc/pgpassfile v1.0.0 // indirect
	github.com/jackc/pgservicefile v0.0.0-20240606120523-5a60cdf6a761 // indirect
	github.com/jackc/puddle/v2 v2.2.3 // indirect
	golang.org/x/sync v0.24.0 // indirect
	golang.org/x/text v0.43.0 // indirect
)
