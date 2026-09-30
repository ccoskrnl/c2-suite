// Package session 维护 Agent/CLI 会话注册表与心跳超时扫描。
package session

import (
	"errors"
	"net"
	"sync"
	"time"

	"ratc2/internal/protocol"
)

// 心跳参数（docs/protocol.md §6）
const (
	HeartbeatInterval = 15 * time.Second // Agent 心跳周期
	OfflineAfter      = 30 * time.Second // 无包判定离线
	ReapAfter         = 60 * time.Second // 离线后回收
)

// Role 与协议一致
type Role uint8

const (
	RoleAgent   Role = 0
	RoleControl Role = 1
)

// Session 一个 Agent（或被控侧）会话。
type Session struct {
	ID       string // agent_id 的 hex 形式（小写）
	Role     Role
	Key      []byte // 派生会话密钥
	IP       string
	Port     uint16
	OSInfo   string
	Plugins  uint32
	LastSeen time.Time
	Online   bool

	mu    sync.Mutex
	conn  net.Conn // 底层连接（sweep 关闭用）
	seq   uint32   // C2 侧转发序号（发往 agent 的包）
	seqMu sync.Mutex
	Beacon bool   // true = HTTP 轮询信道（§8），超时阈值按轮询周期计算
}

// NextSeq 返回下一个转发序号（C2 发给 agent 的包使用，严格递增）。
func (s *Session) NextSeq() uint32 {
	s.seqMu.Lock()
	s.seq++
	v := s.seq
	s.seqMu.Unlock()
	return v
}

// WritePacket 用会话密钥加密后的载荷直接写入底层连接（并发安全，读方向由 agentLoop 独占）。
func (s *Session) WritePacket(h *protocol.Header, sealed []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.conn == nil {
		return errors.New("no conn")
	}
	return protocol.WritePacket(s.conn, h, sealed)
}

// SetConn 绑定底层连接（握手完成后调用）。
func (s *Session) SetConn(c net.Conn) {
	s.mu.Lock()
	s.conn = c
	s.mu.Unlock()
}

// Touch 刷新最后活跃时间。
func (s *Session) Touch() {
	s.mu.Lock()
	s.LastSeen = time.Now()
	s.Online = true
	s.mu.Unlock()
}

// SetOnline 标记在线/离线。
func (s *Session) SetOnline(v bool) {
	s.mu.Lock()
	s.Online = v
	s.mu.Unlock()
}

// Close 关闭底层连接。
func (s *Session) Close() {
	s.mu.Lock()
	if s.conn != nil {
		s.conn.Close()
	}
	s.mu.Unlock()
}

// Registry 会话注册表（并发安全）。
type Registry struct {
	mu    sync.RWMutex
	byID  map[string]*Session
	byKey map[string]*Session // 连接标识 -> 会话（供离线清理）
}

func NewRegistry() *Registry {
	return &Registry{
		byID:  make(map[string]*Session),
		byKey: make(map[string]*Session),
	}
}

// Register 注册会话；若 agent_id 冲突，返回旧的（由调用方踢下线）。
func (r *Registry) Register(s *Session) *Session {
	r.mu.Lock()
	defer r.mu.Unlock()
	old := r.byID[s.ID]
	r.byID[s.ID] = s
	r.byKey[connKey(s.conn)] = s
	return old
}

// Unregister 移除会话。
func (r *Registry) Unregister(s *Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.byID[s.ID] == s {
		delete(r.byID, s.ID)
	}
	delete(r.byKey, connKey(s.conn))
}

// Get 按 agent_id 查会话。
func (r *Registry) Get(id string) *Session {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.byID[id]
}

// ListAgents 返回全部 agent 会话（快照）。
func (r *Registry) ListAgents() []*Session {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]*Session, 0, len(r.byID))
	for _, s := range r.byID {
		if s.Role == RoleAgent {
			out = append(out, s)
		}
	}
	return out
}

// SweepLoop 周期扫描：超时标记离线，再超时回收并关闭连接。
// Beacon 信道（§8.6）按 3x/6x sleeptime（默认 60s → 180s/360s）判定。
func (r *Registry) SweepLoop(interval time.Duration) {
	for range time.Tick(interval) {
		now := time.Now()
		r.mu.Lock()
		for id, s := range r.byID {
			offlineAfter, reapAfter := OfflineAfter, ReapAfter
			if s.Beacon {
				offlineAfter, reapAfter = 180*time.Second, 360*time.Second
			}
			last := s.LastSeen
			if now.Sub(last) > reapAfter {
				if s.conn != nil {
					s.conn.Close()
				}
				delete(r.byID, id)
				delete(r.byKey, connKey(s.conn))
			} else if now.Sub(last) > offlineAfter {
				s.Online = false
			}
		}
		r.mu.Unlock()
	}
}

func connKey(c net.Conn) string {
	if c == nil {
		return ""
	}
	return c.RemoteAddr().String()
}
