import AVFoundation

/// 导航播报档位（存 UserDefaults "voice_mode"，默认 0 详细：老用户升级后行为完全不变）。
///
/// 高德 SDK 通过 driveManager:playNaviSoundString: 把「播报文字」交给我们，
/// App 再用系统语音念出来（见 Speech.speak）。SDK 的 setBroadcastMode: 只有
/// 简洁 / 详细两档，且导航中改档要「下次算路」才生效；这里改为在 App 侧对高德
/// 回传的文字做分级处理：三档即时生效、无需改 SDK 调用，也不碰导航数据链路。
enum BroadcastMode: Int, CaseIterable, Identifiable {
    case detailed = 0      // 详细：高德回传什么就念什么（原有行为）
    case concise  = 1      // 简洁：跳过安全设施 / 路况 / 服务区等次要提醒
    case minimal  = 2      // 极简：只念转向 / 到达等核心指令，并剥离套话（参考高德极简模式）
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .detailed: return "详细播报"
        case .concise:  return "简洁播报"
        case .minimal:  return "极简播报"
        }
    }
    var note: String {
        switch self {
        case .detailed: return "完整念出高德下发的每一句提示，信息最全（原有行为）。"
        case .concise:  return "略过学校 / 事故多发 / 施工 / 路况 / 服务区等次要提醒，保留转向、测速限速与到达。"
        case .minimal:  return "只念左转 / 右转 / 直行 / 掉头 / 到达等核心指令，跳过电子眼、路况、车道等次要播报（参考高德极简模式）；例外：超速通过前方测速点时提前 500 米提醒一次。"
        }
    }
    /// 读当前设置；键不存在返回 0 = 详细，保证升级后行为不变
    static var current: BroadcastMode {
        BroadcastMode(rawValue: UserDefaults.standard.integer(forKey: "voice_mode")) ?? .detailed
    }
}

/// 极简档唯一的例外播报：前方测速点超速提醒（纯逻辑，便于回归测试）。
/// 触发：车速 > 测速限速 × 1.08，且距测速点 ≤ 500 米；同一个测速点只提醒一次。
/// 极简档会过滤掉高德的所有测速播报，这条由 NaviBridge 在满足条件时直接调用
/// Speech.speakAlert 补上（绕过档位文字过滤，但仍受「是否播报」静音开关控制）。
enum OverspeedRule {
    static let ratio: Double = 1.08
    static let leadMeters = 500

    static func shouldAlert(minimal: Bool, speedKmh: Int, limit: Int,
                            distance: Int, alerted: Bool) -> Bool {
        guard minimal, !alerted, limit > 0, speedKmh > 0,
              distance >= 0, distance <= leadMeters else { return false }
        return Double(speedKmh) > Double(limit) * ratio
    }

    /// 文案按实际距离取整到 10 米（和圆屏测速牌一致），不用固定「五百米」以免报错距离。
    static func text(speedKmh: Int, limit: Int, distance: Int) -> String {
        let d = max(0, (distance + 5) / 10 * 10)
        return "前方\(d)米有测速，限速\(limit)公里，当前车速\(speedKmh)公里，请减速"
    }
}

/// 版本：跟随 App 版本（见 Config.swift）
///
/// 导航播报：高德把播报文字交给我们，用 iPhone 自带的语音念出来。
/// 播报时把音乐压低（“压低音乐”开关），念完 0.4 秒后恢复；连着蓝牙耳机/头盔时从耳机里出声。
/// 播报音量 0~200%：0~100% 直接给 AVSpeechUtterance.volume（系统上限 1.0）；
/// 100% 以上由 GainPlayer 对语音 PCM 做软件增益（最多 2 倍），不动系统媒体音量。
final class Speech: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = Speech()
    private let synth = AVSpeechSynthesizer()
    private let gainPlayer = GainPlayer()             // 只在音量 >100% 时使用
    private var automaticVoice: AVSpeechSynthesisVoice?
    private var busy = 0
    private var releaseWork: DispatchWorkItem?
    private(set) var isSpeaking = false

    override init() {
        super.init()
        synth.delegate = self
        automaticVoice = Self.preferredChineseVoice()
    }

    /// 只自动选择普通话。用 identifier 稳定排序，避免同级音色随枚举顺序切换。
    private static func preferredChineseVoice() -> AVSpeechSynthesisVoice? {
        let voices = AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.language == "zh-CN" }
            .sorted { $0.identifier < $1.identifier }
        if #available(iOS 16.0, *) {
            if let premium = voices.first(where: { $0.quality == .premium }) {
                return premium
            }
        }
        return voices.first(where: { $0.quality == .enhanced })
            ?? AVSpeechSynthesisVoice(language: "zh-CN")
    }

    private func selectedVoice() -> AVSpeechSynthesisVoice? {
        let id = UserDefaults.standard.string(forKey: "voice_id") ?? ""
        if !id.isEmpty, let selected = AVSpeechSynthesisVoice(identifier: id) {
            return selected
        }
        // 每次自动播报前刷新，下载／删除系统音色后无需重装或修改设置。
        // 不把自动结果写入 voice_id，以便以后继续自动升级音色。
        automaticVoice = Self.preferredChineseVoice()
        return automaticVoice
    }

    private func configureSession(active: Bool) {
        let s = AVAudioSession.sharedInstance()
        do {
            if active {
                let opts: AVAudioSession.CategoryOptions = Settings.shared.duck
                    ? [.duckOthers, .interruptSpokenAudioAndMixWithOthers]
                    : [.mixWithOthers]
                try s.setCategory(.playback, mode: .voicePrompt, options: opts)
                try s.setActive(true)
            } else {
                try s.setActive(false, options: .notifyOthersOnDeactivation)   // 让音乐恢复音量
            }
        } catch {
            print("音频设置失败：\(error)")
        }
    }

    // MARK: - 播报档位：对高德回传文字做分级过滤（纯字符串逻辑，便于回归测试）

    /// 含这些词 = 转向 / 到达等「必须念」的核心指令
    private static let coreWords = [
        "左转", "右转", "掉头", "左前方", "右前方", "左后方", "右后方",
        "靠左", "靠右", "直行", "到达", "目的地", "终点", "环岛", "匝道",
        "驶入", "并入主路", "前方路口", "前方红绿灯"
    ]
    /// 简洁档跳过：不含核心指令、且属于安全设施 / 路况 / 服务区的次要提醒
    private static let conciseSkipWords = [
        "学校", "村庄", "事故多发", "落石", "易滑", "急弯", "连续弯道", "路面不平",
        "注意安全", "确保安全", "小心驾驶", "减速慢行", "谨慎驾驶", "避让行人",
        "注意避让", "施工", "拥堵", "畅通", "路况", "服务区", "收费站", "停车区",
        "隧道", "桥梁", "陡坡", "注意", "保持车距", "请按导向车道"
    ]
    /// 极简档剥离的套话（整段短语，不会切坏核心指令）
    private static let trimPhrases = [
        "请注意安全", "请小心驾驶", "请确保安全", "注意安全", "减速慢行",
        "谨慎驾驶", "小心驾驶", "注意避让行人", "请避让行人", "保持安全车距",
        "沿当前道路行驶", "请按导向车道行驶", "确保行车安全", "注意观察",
        "注意", "请"
    ]

    /// 按档位处理高德回传的一句播报文字：返回 nil 表示该档位下这句不念。
    static func filtered(_ raw: String, mode: BroadcastMode) -> String? {
        let t = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.isEmpty { return nil }
        switch mode {
        case .detailed:
            return t                                   // 详细：原样，行为与旧版完全一致
        case .concise, .minimal:
            let core = Self.coreWords.contains { t.contains($0) }
            if core {
                return mode == .minimal ? Self.strip(t) : t    // 核心指令：保留；极简再剥离套话
            }
            if mode == .minimal { return nil }          // 极简：非核心指令一律不念
            if Self.conciseSkipWords.contains(where: { t.contains($0) }) { return nil }
            return t                                    // 简洁：其余（测速 / 限速 / 到达预告等）保留
        }
    }

    /// 极简档：删掉固定套话，再按中文逗号 / 句号分段、丢掉空段后拼回
    static func strip(_ t: String) -> String {
        var s = t
        for p in Self.trimPhrases { s = s.replacingOccurrences(of: p, with: "") }
        let parts = s.components(separatedBy: CharacterSet(charactersIn: "，。"))
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        return parts.joined(separator: "，")
    }

    /// App 内独立播报音量（0~2 = 0%~200%，存 UserDefaults "voice_volume"，默认 1.0 = 100%）。
    /// 0~1 直接给 AVSpeechUtterance.volume（它的上限就是 1.0）；1~2 的超额部分由
    /// GainPlayer 对语音 PCM 做软件增益，所以 200% 是真实的两倍幅度（+6.02 dB）。
    /// 与音乐音量互不干扰；但 iOS 系统「媒体音量」仍是总闸门（系统音量调到 0 时一定没声）。
    private var volume: Double {
        let v = UserDefaults.standard.object(forKey: "voice_volume") as? Double ?? 1.0
        return min(max(v, 0.0), 2.0)
    }

    func speak(_ text: String) {
        if Settings.shared.voiceMuted { return }        // iOS v0.6 导航画面上点了静音
        // 按「导航语音」里选的档位过滤高德回传的文字；该档位下不念就直接返回，
        // 不激活音频会话、也不占用播报通道。
        guard let t = Self.filtered(text, mode: BroadcastMode.current) else { return }
        emit(t)
    }

    /// 极简档的超速测速提醒：由 NaviBridge 直接调用，绕过档位文字过滤
    /// （否则「测速 / 限速」会被极简档判为非核心指令过滤掉），静音开关仍然生效。
    func speakAlert(_ text: String) {
        if Settings.shared.voiceMuted { return }
        emit(text)
    }

    private func makeUtterance(_ t: String) -> AVSpeechUtterance {
        let u = AVSpeechUtterance(string: t)
        u.voice = selectedVoice()
        u.rate = Float(UserDefaults.standard.object(forKey: "voice_rate") as? Double ?? Double(AVSpeechUtteranceDefaultSpeechRate))
        u.pitchMultiplier = Float(UserDefaults.standard.object(forKey: "voice_pitch") as? Double ?? 1.0)
        return u
    }

    /// 真正占用播报通道。≤100% 走原来的 AVSpeechSynthesizer.speak（行为完全不变）；
    /// >100% 因为 AVSpeechUtterance.volume 上限只有 1.0，改走 GainPlayer 做 PCM 软件增益，
    /// 增益路径任何一步失败都会自动回退到普通播报（音量 1.0），保证这句不会丢。
    private func emit(_ t: String) {
        releaseWork?.cancel()
        if busy == 0 { configureSession(active: true) }
        busy += 1
        isSpeaking = true
        let g = volume
        if g <= 1.0 {
            let u = makeUtterance(t)
            u.volume = Float(g)                        // App 内独立音量，不影响音乐
            synth.speak(u)
        } else {
            let u = makeUtterance(t)
            u.volume = 1.0                            // 原始音量不衰减，超额部分在 PCM 里放大
            gainPlayer.play(u, gain: Float(g),
                            onDone: { [weak self] in
                                guard let self = self else { return }
                                self.finished()
                            },
                            onFallback: { [weak self] in
                                guard let self = self else { return }
                                let f = self.makeUtterance(t)
                                f.volume = 1.0
                                self.synth.speak(f)    // delegate 的 didFinish 会调用 finished()
                            })
        }
    }

    private func finished() {
        busy = max(0, busy - 1)
        if busy == 0 {
            let w = DispatchWorkItem { [weak self] in
                guard let self = self, self.busy == 0 else { return }
                self.isSpeaking = false
                self.configureSession(active: false)
            }
            releaseWork = w
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: w)
        }
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) { DispatchQueue.main.async { self.finished() } }
    func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) { DispatchQueue.main.async { self.finished() } }

    /// 马上停止正在念的（点了静音）
    func stopNow() {
        synth.stopSpeaking(at: .immediate)
        gainPlayer.cancel()                            // >100% 的增益播放也要立刻停，并归还播报通道
    }

    /// 试听（不管静音开关）
    func test() {
        let m = Settings.shared.voiceMuted
        Settings.shared.voiceMuted = false
        speak("前方三百米右转，进入人民南路。注意，前方有测速，限速六十。")  // 试听也走当前档位过滤
        Settings.shared.voiceMuted = m
    }
}

/// >100% 音量的软件增益播放（AVSpeechUtterance.volume 上限只有 1.0，给 2.0 也会被钳回 1.0）。
///
/// 做法：用 AVSpeechSynthesizer.write(_:toBufferCallback:)（iOS 13+）离线合成出 PCM，
/// 逐样本乘 gain（1~2，超过 1 的部分限幅防爆音），拼成 16-bit 单声道 WAV，
/// 交给 AVAudioPlayer 播放。200% 是真实的两倍幅度，且完全不动系统媒体音量。
///
/// 所有状态只在主线程读写：write 的回调不管在哪个线程，都先 dispatch 回主线程。
/// ≤100% 完全不碰这里（沿用 synth.speak，行为逐字不变），只有音量拉过 100% 才走这条路径；
/// 任何环节失败自动回退到普通播报（音量 1.0），保证这句不会丢。
private final class GainPlayer: NSObject, AVAudioPlayerDelegate {
    private let writeSynth = AVSpeechSynthesizer()
    private var generation = 0
    private var active = false
    private var flushed = false
    private var samples: [Float] = []
    private var sampleRate: Double = 0
    private var player: AVAudioPlayer?
    private var onDone: (() -> Void)?
    private var onFallback: (() -> Void)?

    func play(_ u: AVSpeechUtterance, gain: Float,
              onDone: @escaping () -> Void, onFallback: @escaping () -> Void) {
        cancel()                                   // 新的一句来了：丢掉上一句，归还它的播报通道
        generation += 1
        let g = generation
        active = true
        flushed = false
        samples = []
        sampleRate = 0
        self.onDone = onDone
        self.onFallback = onFallback
        u.volume = 1.0
        writeSynth.write(u, toBufferCallback: { [weak self] buf in
            DispatchQueue.main.async {
                guard let self = self, self.active, self.generation == g else { return }
                guard let pcm = GainPlayer.asPCM(buf) else { return }
                if pcm.frameLength == 0 { self.flush(g: g); return }   // 空 buffer = 合成结束
                if !GainPlayer.collect(pcm, gain: gain, into: &self.samples, rate: &self.sampleRate) {
                    self.fallback()                                     // 不支持的 PCM 格式 → 回退
                }
            }
        })
        // 兜底：个别系统版本合成结束不给空 buffer，2 秒后强制收尾，避免这句永远不播
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            guard let self = self, self.active, self.generation == g, !self.samples.isEmpty else { return }
            self.flush(g: g)
        }
    }

    /// 回调参数在不同 SDK 里是 AVAudioBuffer / AVAudioPCMBuffer，统一走这里兼容
    private static func asPCM(_ b: AVAudioBuffer) -> AVAudioPCMBuffer? { b as? AVAudioPCMBuffer }

    /// 混成单声道并乘增益；支持 float32 / int16，别的格式返回 false 让调用方回退
    private static func collect(_ pcm: AVAudioPCMBuffer, gain: Float,
                                into s: inout [Float], rate: inout Double) -> Bool {
        let n = Int(pcm.frameLength)
        guard n > 0 else { return true }
        rate = pcm.format.sampleRate
        let ch = max(1, Int(pcm.format.channelCount))
        if let f = pcm.floatChannelData {
            for i in 0..<n {
                var v: Float = 0
                for c in 0..<ch { v += f[c][i] }
                s.append(limit(v / Float(ch) * gain))
            }
            return true
        }
        if let i16 = pcm.int16ChannelData {
            for i in 0..<n {
                var v: Float = 0
                for c in 0..<ch { v += Float(i16[c][i]) / 32768 }
                s.append(limit(v / Float(ch) * gain))
            }
            return true
        }
        return false
    }

    /// 限幅：gain 最多 2，TTS 峰值一般不高，夹到 [-1,1] 避免削波爆音
    private static func limit(_ v: Float) -> Float { max(-1, min(1, v)) }

    private func flush(g: Int) {
        guard active, generation == g, !flushed else { return }
        flushed = true
        guard !samples.isEmpty, sampleRate > 0 else { fallback(); return }
        let data = GainPlayer.wav(samples, rate: sampleRate)
        samples = []
        do {
            let p = try AVAudioPlayer(data: data)
            player = p
            p.delegate = self
            p.volume = 1.0                       // 增益已在 PCM 里做了，这里保持 1.0
            _ = p.prepareToPlay()
            _ = p.play()
        } catch {
            fallback()
        }
    }

    /// 16-bit 单声道 PCM WAV（手写 44 字节头，纯 Data，不落临时文件）
    private static func wav(_ s: [Float], rate: Double) -> Data {
        var d = Data()
        func tag(_ t: String) { d.append(contentsOf: t.utf8) }
        func u16(_ v: UInt16) { d.append(UInt8(v & 0xff)); d.append(UInt8((v >> 8) & 0xff)) }
        func u32(_ v: UInt32) {
            d.append(UInt8(v & 0xff)); d.append(UInt8((v >> 8) & 0xff))
            d.append(UInt8((v >> 16) & 0xff)); d.append(UInt8((v >> 24) & 0xff))
        }
        let sr = UInt32(max(8000, min(48000, rate)))
        let bytes = UInt32(s.count * 2)
        tag("RIFF"); u32(36 + bytes); tag("WAVE")
        tag("fmt "); u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16)
        tag("data"); u32(bytes)
        for v in s {
            let q = Int32(max(-32768.0, min(32767.0, Double(v * 32767))))
            let bits = UInt16(bitPattern: Int16(q))
            d.append(UInt8(bits & 0xff)); d.append(UInt8((bits >> 8) & 0xff))
        }
        return d
    }

    func audioPlayerDidFinishPlaying(_ p: AVAudioPlayer, successfully flag: Bool) { complete() }
    func audioPlayerDecodeErrorDidOccur(_ p: AVAudioPlayer, error: Error?) { fallback() }

    private func complete() {
        guard active else { return }
        active = false
        player = nil
        onFallback = nil
        let cb = onDone; onDone = nil
        cb?()                                     // 归还播报通道（Speech.finished）
    }

    private func fallback() {
        guard active else { return }
        active = false
        player?.stop()
        player = nil
        onDone = nil
        let cb = onFallback; onFallback = nil
        cb?()                                     // 回退到普通播报，由 synth delegate 归还通道
    }

    /// 静音 / 新播报打断：立刻停掉增益播放，并归还这一句占用的播报通道
    func cancel() {
        generation += 1
        guard active else { return }
        active = false
        let p = player
        player = nil
        let cb = onDone
        onDone = nil; onFallback = nil
        p?.stop()
        samples = []
        cb?()
    }
}
