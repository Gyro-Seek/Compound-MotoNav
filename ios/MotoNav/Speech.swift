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
        case .minimal:  return "只念左转 / 右转 / 直行 / 掉头 / 到达等核心指令，跳过电子眼、路况、车道等所有次要播报（参考高德极简模式）。"
        }
    }
    /// 读当前设置；键不存在返回 0 = 详细，保证升级后行为不变
    static var current: BroadcastMode {
        BroadcastMode(rawValue: UserDefaults.standard.integer(forKey: "voice_mode")) ?? .detailed
    }
}

/// 版本：跟随 App 版本（见 Config.swift）
///
/// 导航播报：高德把播报文字交给我们，用 iPhone 自带的语音念出来。
/// 播报时把音乐压低（“压低音乐”开关），念完 0.4 秒后恢复；连着蓝牙耳机/头盔时从耳机里出声。
final class Speech: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = Speech()
    private let synth = AVSpeechSynthesizer()
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

    /// App 内独立播报音量（0~1，存 UserDefaults "voice_volume"，默认 1.0）。
    /// 只作用于 AVSpeechUtterance.volume，是相对增益：与音乐音量互不干扰；
    /// 但 iOS 系统「媒体音量」仍是总闸门（系统音量调到 0 时一定没声）。
    private var volume: Float {
        let v = UserDefaults.standard.object(forKey: "voice_volume") as? Double ?? 1.0
        return Float(min(max(v, 0.0), 1.0))
    }

    func speak(_ text: String) {
        if Settings.shared.voiceMuted { return }        // iOS v0.6 导航画面上点了静音
        // 按「导航语音」里选的档位过滤高德回传的文字；该档位下不念就直接返回，
        // 不激活音频会话、也不占用播报通道。
        guard let t = Self.filtered(text, mode: BroadcastMode.current) else { return }
        releaseWork?.cancel()
        if busy == 0 { configureSession(active: true) }
        busy += 1
        isSpeaking = true
        let u = AVSpeechUtterance(string: t)
        u.voice = selectedVoice()
        u.rate = Float(UserDefaults.standard.object(forKey: "voice_rate") as? Double ?? Double(AVSpeechUtteranceDefaultSpeechRate))
        u.pitchMultiplier = Float(UserDefaults.standard.object(forKey: "voice_pitch") as? Double ?? 1.0)
        u.volume = volume                              // App 内独立音量，不影响音乐
        synth.speak(u)
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
    func stopNow() { synth.stopSpeaking(at: .immediate) }

    /// 试听（不管静音开关）
    func test() {
        let m = Settings.shared.voiceMuted
        Settings.shared.voiceMuted = false
        speak("前方三百米右转，进入人民南路。注意，前方有测速，限速六十。")  // 试听也走当前档位过滤
        Settings.shared.voiceMuted = m
    }
}
