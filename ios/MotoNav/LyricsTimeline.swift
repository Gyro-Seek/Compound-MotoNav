import Foundation

struct LyricsTrack: Equatable {
    let title: String, artist: String, album: String
    let duration: Double
    var key: String { "\(title)\n\(artist)\n\(album)\n\(duration)" }
    var url: URL? {
        guard !title.isEmpty, !artist.isEmpty else { return nil }
        // 时长有效走精确查询；网易云/QQ 音乐经 AMS 常不提供时长，改走 /api/search 按歌名+歌手查找，不再直接放弃。
        let exact = duration.isFinite && (1...3600).contains(duration)
        var url = URLComponents(string: exact ? "https://lrclib.net/api/get" : "https://lrclib.net/api/search")!
        var items = [URLQueryItem(name: "track_name", value: title), URLQueryItem(name: "artist_name", value: artist)]
        if exact { items.append(URLQueryItem(name: "duration", value: String(duration))) }
        if !album.isEmpty { items.append(URLQueryItem(name: "album_name", value: album)) }
        url.queryItems = items
        return url.url
    }
    func matches(title: String, artist: String, album: String, duration: Double) -> Bool {
        func normalized(_ text: String) -> String {
            text.folding(options: [.caseInsensitive, .diacriticInsensitive, .widthInsensitive], locale: Locale(identifier: "en_US_POSIX"))
                .unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) }.map(String.init).joined()
        }
        // 时长未知(<=0, 来自 /api/search)时不校验时长；中文歌曲副标题差异常见，标题/歌手允许包含关系。
        let durationOK = self.duration <= 0 || duration <= 0 || !duration.isFinite || abs(self.duration - duration) <= 3
        let t = normalized(self.title), a = normalized(self.artist), rt = normalized(title), ra = normalized(artist)
        guard !rt.isEmpty, !ra.isEmpty else { return false }
        return durationOK && (t == rt || t.contains(rt) || rt.contains(t))
            && (a == ra || a.contains(ra) || ra.contains(a))
            && (self.album.isEmpty || album.isEmpty || normalized(self.album) == normalized(album))
    }
}

struct MediaPlaybackClock {
    let state: Int, rate: Double, elapsed: Double, anchor: TimeInterval
    init?(_ text: String, now: TimeInterval) {
        let p = text.split(separator: ",", omittingEmptySubsequences: false)
        guard p.count == 3, let state = Int(p[0]), (0...3).contains(state), let rate = Double(p[1]), rate.isFinite,
              let elapsed = Double(p[2]), elapsed.isFinite, elapsed >= 0 else { return nil }
        self.state = state; self.rate = rate; self.elapsed = elapsed; anchor = now
    }
    func position(now: TimeInterval, duration: Double) -> Double {
        // Paused clocks stay fixed even if the player supplied an old non-zero rate.
        let movement = state == 0 ? 0 : rate * max(0, now - anchor)
        return min(max(0, elapsed + movement), max(0, duration))
    }
}

struct LyricsTimeline {
    struct Cue: Equatable { let time: Double; let text: String }
    let cues: [Cue]
    init(_ lrc: String) {
        guard lrc.utf8.count <= 128 * 1024 else { cues = []; return }
        let stamp = try! NSRegularExpression(pattern: #"^\[(\d{1,3}):([0-5]\d)(?:[.:](\d{1,3}))?\]"#)
        let offsetTag = try! NSRegularExpression(pattern: #"\[offset:([+-]?\d+)\]"#, options: .caseInsensitive)
        var offset: Double = 0
        if let m = offsetTag.firstMatch(in: lrc, range: NSRange(lrc.startIndex..., in: lrc)),
           let r = Range(m.range(at: 1), in: lrc), let milliseconds = Double(lrc[r]), abs(milliseconds) <= 300_000 { offset = milliseconds / 1000 }
        var lines: [(Double, Int, String)] = []
        for (order, raw) in lrc.components(separatedBy: .newlines).prefix(3000).enumerated() {
            var line = raw.trimmingCharacters(in: .whitespaces); var times: [Double] = []
            while let m = stamp.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)), times.count < 32 {
                let ns = line as NSString
                let minutes = Double(ns.substring(with: m.range(at: 1)))!, seconds = Double(ns.substring(with: m.range(at: 2)))!
                var fraction = 0.0
                if m.range(at: 3).location != NSNotFound { let digits = ns.substring(with: m.range(at: 3)); fraction = Double(digits)! / pow(10, Double(digits.count)) }
                times.append(max(0, minutes * 60 + seconds + fraction + offset))
                line = ns.substring(from: m.range.length)
            }
            let text = String(line.trimmingCharacters(in: .whitespaces).prefix(400))
            for time in times { lines.append((time, order, text)) }
        }
        lines.sort { $0.0 == $1.0 ? $0.1 < $1.1 : $0.0 < $1.0 }
        var merged: [Cue] = []
        for (time, _, text) in lines {
            if let last = merged.last, last.time == time {
                if !text.isEmpty && !last.text.components(separatedBy: " / ").contains(text) {
                    merged[merged.count - 1] = Cue(time: time, text: last.text.isEmpty ? text : last.text + " / " + text)
                }
            } else { merged.append(Cue(time: time, text: text)) }
        }
        cues = merged
    }
    func line(at seconds: Double) -> String {
        guard seconds.isFinite, seconds >= 0 else { return "" }
        var low = 0, high = cues.count
        while low < high { let mid = (low + high) / 2; if cues[mid].time <= seconds { low = mid + 1 } else { high = mid } }
        return low > 0 ? cues[low - 1].text : ""
    }

    /// 三行歌词上下文：上一句 / 当前句 / 下一句。间奏空行原样返回空串（固件清掉该槽）。
    func context(at seconds: Double) -> (prev: String, current: String, next: String) {
        guard seconds.isFinite, seconds >= 0 else { return ("", "", "") }
        var low = 0, high = cues.count
        while low < high { let mid = (low + high) / 2; if cues[mid].time <= seconds { low = mid + 1 } else { high = mid } }
        let i = low > 0 ? low - 1 : -1
        func text(_ j: Int) -> String { cues.indices.contains(j) ? cues[j].text : "" }
        return (i > 0 ? text(i - 1) : "", i >= 0 ? text(i) : "", i >= 0 ? text(i + 1) : "")
    }
}
