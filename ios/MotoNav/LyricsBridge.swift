import Foundation

/// Optional third-party lyrics. Display is deliberately decoupled from HID/AMS
/// availability: as long as the "逐句歌词" toggle is on and BLE is ready, the round
/// screen always shows either the current line, a static lyric line, or a short
/// Chinese status — never a blank lyrics area with no explanation.
final class LyricsBridge {
    static let shared = LyricsBridge()
    private struct Record: Decodable {
        let trackName: String?, artistName: String?, albumName: String?, duration: Double?
        let instrumental: Bool?, syncedLyrics: String?
    }
    private struct Cached { var timeline: LyricsTimeline?; var status: String; var expires: TimeInterval }
    private var cache: [String: Cached] = [:]
    private var key = "", token = UUID(), task: URLSessionDataTask?, work: DispatchWorkItem?, timer: Timer?
    private var titleRevision: UInt16 = 0, artistRevision: UInt16 = 0
    private var generation: UInt32?, playbackRevision: UInt16 = 0, clock: MediaPlaybackClock?
    private var track: LyricsTrack?, timeline: LyricsTimeline?, lastLine: String?, lyricsEnabled = false
    private var systemPlaying: Bool?

    /// Push a short Chinese status onto slot 12 instead of leaving the area blank.
    private func showStatus(_ text: String) {
        BleLink.shared.mediaLyricsStatus = text
        sendLine(text)
    }

    /// AMS / HID path: metadata is relayed from the round screen.
    func update(_ metadata: AMSMetadata, force: Bool = false) {
        // Drop the old musicHID / mediaLyricsSupported hard gates. A firmware that
        // never announces CAP,LYR,1 (or a phone with media keys switched off) must
        // not silently disable lyrics: old firmware simply ignores slot 12.
        guard Settings.shared.musicLyrics, BleLink.shared.isReady else { stop(); return }
        lyricsEnabled = true
        systemPlaying = nil
        if generation != metadata.generation { resetTrack(); generation = metadata.generation; playbackRevision = 0; clock = nil }
        if titleRevision != metadata.values[0].revision || artistRevision != metadata.values[1].revision {
            titleRevision = metadata.values[0].revision; artistRevision = metadata.values[1].revision; lastLine = nil
        }
        if playbackRevision != metadata.values[4].revision {
            playbackRevision = metadata.values[4].revision
            clock = MediaPlaybackClock(metadata.values[4].text, now: ProcessInfo.processInfo.systemUptime)
        }
        guard metadata.ready, !metadata.title.isEmpty, !metadata.artist.isEmpty,
              !metadata.values[0].partial, !metadata.values[1].partial else {
            resetTrack(); showStatus("等待完整歌曲信息"); return
        }
        // Duration is optional now: LRCLIB falls back to /api/search when missing.
        let duration = Double(metadata.values[3].text).flatMap { $0.isFinite && (1...3600).contains($0) ? $0 : nil } ?? 0
        let incoming = LyricsTrack(title: metadata.title, artist: metadata.artist, album: metadata.album, duration: duration)
        startTrack(incoming, force: force)
    }

    /// App-direct path (media keys OFF): the system Music player supplies the track.
    func updateSystem(title: String, artist: String, album: String, duration: Double, elapsed: Double, playing: Bool) {
        guard Settings.shared.musicLyrics, BleLink.shared.isReady else { stop(); return }
        guard !title.isEmpty else { resetTrack(); showStatus("请先在音乐 App 播放歌曲"); return }
        lyricsEnabled = true
        generation = nil
        let d = duration.isFinite && (1...3600).contains(duration) ? duration : 0
        let incoming = LyricsTrack(title: title, artist: artist, album: album, duration: d)
        let changed = incoming.key != key
        startTrack(incoming, force: changed)
        if changed || clock == nil || systemPlaying != playing {
            clock = MediaPlaybackClock("\(playing ? 1 : 0),\(playing ? 1 : 0),\(max(0, elapsed))",
                                       now: ProcessInfo.processInfo.systemUptime)
        }
        systemPlaying = playing
        tick()
    }

    private func startTrack(_ incoming: LyricsTrack, force: Bool) {
        if incoming.key != key {
            resetTrack(); key = incoming.key; track = incoming
            showStatus("正在匹配逐句歌词…")
            let current = token
            let item = DispatchWorkItem { [weak self] in
                guard let self = self, self.token == current else { return }
                self.fetch(incoming, token: current)
            }
            work = item; DispatchQueue.main.asyncAfter(deadline: .now() + 0.8, execute: item)
        }
        if force { lastLine = nil }
        if timer == nil { timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.tick() } }
        tick()
    }

    func stop() {
        if lyricsEnabled { resetTrack(); timer?.invalidate(); timer = nil; lyricsEnabled = false }
        generation = nil; playbackRevision = 0; clock = nil; systemPlaying = nil
        BleLink.shared.mediaLyricsStatus = ""
    }

    private func resetTrack() {
        token = UUID(); task?.cancel(); task = nil; work?.cancel(); work = nil; key = ""; track = nil; timeline = nil
        sendLine("")
    }

    private func fetch(_ track: LyricsTrack, token: UUID) {
        if let cached = cache[track.key], cached.expires > Date().timeIntervalSince1970 { apply(cached, track: track, token: token); return }
        guard let url = track.url else { showStatus("歌曲信息不完整，无法匹配歌词"); return }
        var request = URLRequest(url: url); request.timeoutInterval = 12
        request.setValue("MotoNav/0.12.1 (iOS; optional synced lyrics)", forHTTPHeaderField: "User-Agent")
        task = URLSession.shared.dataTask(with: request) { [weak self] data, response, error in
            var result = Cached(timeline: nil, status: "歌词服务暂不可用，保留歌名和歌手", expires: Date().timeIntervalSince1970 + 60)
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            if code == 404 { result.status = "未找到匹配的逐句歌词"; result.expires += 540 }
            else if error == nil, code == 200, let data = data, data.count <= 512 * 1024 {
                // /api/get returns a single object; /api/search returns an array.
                let records: [Record] = (try? JSONDecoder().decode([Record].self, from: data))
                    ?? (try? JSONDecoder().decode(Record.self, from: data)).map { [$0] } ?? []
                if let record = records.first(where: { !($0.syncedLyrics ?? "").isEmpty }) ?? records.first,
                   track.matches(title: record.trackName ?? "", artist: record.artistName ?? "",
                                 album: record.albumName ?? "", duration: record.duration ?? 0) {
                    if record.instrumental == true { result.status = "纯音乐，没有歌词" }
                    else if let lrc = record.syncedLyrics {
                        let timeline = LyricsTimeline(lrc)
                        if timeline.cues.contains(where: { !$0.text.isEmpty }) {
                            result = Cached(timeline: timeline, status: "逐句歌词已就绪 · LRCLIB", expires: Date().timeIntervalSince1970 + 86400)
                        } else { result.status = "此歌曲暂无逐句歌词" }
                    } else { result.status = "此歌曲暂无逐句歌词" }
                    if result.timeline == nil { result.expires += 540 }
                }
            }
            let resolved = result
            DispatchQueue.main.async {
                guard let self = self, self.token == token, self.key == track.key, self.lyricsEnabled else { return }
                if self.cache.count >= 20, let oldest = self.cache.min(by: { $0.value.expires < $1.value.expires })?.key { self.cache.removeValue(forKey: oldest) }
                self.cache[track.key] = resolved; self.apply(resolved, track: track, token: token)
            }
        }
        task?.resume()
    }

    private func apply(_ result: Cached, track: LyricsTrack, token: UUID) {
        guard self.token == token, key == track.key, lyricsEnabled else { return }
        timeline = result.timeline; BleLink.shared.mediaLyricsStatus = result.status; tick()
    }

    private func tick() {
        guard lyricsEnabled else { return }
        if let timeline = timeline {
            let dur = track?.duration ?? 0
            if let clock = clock {
                let line = timeline.line(at: clock.position(now: ProcessInfo.processInfo.systemUptime, duration: dur > 0 ? dur : 3600))
                sendLine(line.isEmpty ? (timeline.cues.first { !$0.text.isEmpty }?.text ?? "") : line)
            } else {
                // No playback clock: show the first non-empty line as static lyrics.
                sendLine(timeline.cues.first { !$0.text.isEmpty }?.text ?? "")
            }
        } else {
            // No timeline yet: show WHY instead of pushing an empty image.
            let status = BleLink.shared.mediaLyricsStatus
            sendLine(status.isEmpty ? "正在匹配逐句歌词…" : status)
        }
    }

    private func sendLine(_ text: String) {
        guard lastLine != text else { return }; lastLine = text
        if !text.isEmpty {
            // #LY travels as a text command; slot 12 is a binary image. Both are
            // enqueued in order so the firmware never sees an unannounced image.
            BleLink.shared.sendOnce("#LY,\(generation ?? 1),\(titleRevision),\(artistRevision)")
        }
        TextImage.send(slot: 12, String(text.prefix(200)), maxW: 360, size: 22, minSize: 16)
    }
}
