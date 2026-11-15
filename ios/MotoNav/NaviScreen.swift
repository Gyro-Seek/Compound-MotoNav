import SwiftUI
import ObjectiveC

/// 版本：跟随 App 版本（见 Config.swift）
///
/// 导航时手机上显示的高德原版导航界面（地图、路线、转向提示都由高德画）。
/// 我们的 NaviBridge 同时在旁边“旁听”导航数据发给圆屏。
/// iOS v0.5.2：
/// - 高德界面自带的“关闭”按钮 = 退出导航；“更多”按钮弹出菜单（看全程 / 回到跟随、路况开关、退出导航）
/// - 地图跟着夜间配色切换白天 / 夜间样式
/// - 到达目的地后显示 4 秒“已到达”，然后自动关掉（圆屏同时显示到达，4 秒后回到待机）
/// iOS v0.6：播报静音按钮（下次还记得）；模拟导航暂停 / 继续、车速 40/60/100/150；
/// 退出导航统一用高德界面自带的“关闭”按钮（弹确认）或“更多”菜单；
/// 不再叠加自定义的大按钮，避免与高德自带底栏重叠
/// iOS v0.16.3：浮层控制改为车速圈正下方竖排两个圆形玻璃图标按钮（先设置后播报）；
/// 播报按钮即「是否播报」开关，不再弹出档位选项浮层

/// 接住高德导航界面的按钮回调（和 NaviBridge 一样在运行时登记协议，见 CLAUDE.md 高德坑第 1 条）
final class DriveViewController: NSObject, ObservableObject {
    weak var view: AMapNaviDriveView?
    var onClose: (() -> Void)?
    @Published var showMore = false

    var asDelegate: AMapNaviDriveViewDelegate? {
        class_addProtocol(DriveViewController.self, AMapNaviDriveViewDelegate.self)
        return (self as AnyObject) as? AMapNaviDriveViewDelegate
    }

    @objc(driveViewCloseButtonClicked:)
    func closeTapped(_ v: AMapNaviDriveView) { onClose?() }

    @objc(driveViewMoreButtonClicked:)
    func moreTapped(_ v: AMapNaviDriveView) { showMore = true }

    var isOverview: Bool { view?.showMode == .overview }
    func toggleOverview() {
        guard let v = view else { return }
        v.showMode = v.showMode == .overview ? .carPositionLocked : .overview
    }
    var trafficOn: Bool { view?.mapShowTraffic ?? true }
    func toggleTraffic() { view?.mapShowTraffic.toggle() }
}

struct AMapDriveViewRep: UIViewRepresentable {
    let controller: DriveViewController
    let night: Bool

    func makeUIView(context: Context) -> AMapNaviDriveView {
        let v = AMapNaviDriveView(frame: .zero)
        v.delegate = controller.asDelegate
        v.mapViewModeType = night ? .night : .day
        controller.view = v
        AMapNaviDriveManager.sharedInstance().addDataRepresentative(v)
        return v
    }

    func updateUIView(_ v: AMapNaviDriveView, context: Context) {
        let mode: AMapNaviViewMapModeType = night ? .night : .day
        if v.mapViewModeType != mode { v.mapViewModeType = mode }
    }

    static func dismantleUIView(_ uiView: AMapNaviDriveView, coordinator: ()) {
        AMapNaviDriveManager.sharedInstance().removeDataRepresentative(uiView)
    }
}

struct NaviScreen: View {
    @ObservedObject var navi = NaviBridge.shared
    @ObservedObject var settings = Settings.shared
    @StateObject private var drive = DriveViewController()
    @Environment(\.dismiss) private var dismiss
    @State private var confirmClose = false

    var body: some View {
        ZStack(alignment: .bottom) {
            AMapDriveViewRep(controller: drive, night: settings.paletteNight).ignoresSafeArea()
            VStack(spacing: 10) {
                if navi.uiSuggestMin > 0 {                  // iOS v0.2 高德发现更快的路线
                    HStack {
                        Text("发现更快路线，能省约 \(navi.uiSuggestMin) 分钟")
                            .font(.subheadline).foregroundColor(.white)
                        Spacer()
                        Button("换这条") { navi.acceptSuggest() }
                            .font(.headline).foregroundColor(Color(red: 0.26, green: 0.88, blue: 0.94))
                        Button("不用") { navi.clearSuggest() }
                            .font(.subheadline).foregroundColor(.gray)
                    }
                    .padding(14)
                    .background(RoundedRectangle(cornerRadius: 14).fill(Color.black.opacity(0.85)))
                    .padding(.horizontal, 16)
                }
                if !navi.showArrived {
                    controlStack
                        // 车速圈正下方竖排两个圆形玻璃按钮；留出高德自带底栏的空间
                        .padding(.bottom, glassButtonBottom)
                }
            }
            if navi.showArrived { arrivedCard }
        }
        .onAppear { drive.onClose = { confirmClose = true } }
        .alert("退出导航？", isPresented: $confirmClose) {
            Button("退出导航", role: .destructive) { exitNavi() }
            Button("继续导航", role: .cancel) {}
        }
        .onChange(of: navi.navigating) { now in
            if !now && !navi.showArrived { dismiss() }          // 退出导航：马上关；到达：等“已到达”显示完
        }
        .onChange(of: navi.showArrived) { now in
            if !now && !navi.navigating { dismiss() }           // “已到达”显示完了
        }
        .confirmationDialog("导航选项", isPresented: $drive.showMore, titleVisibility: .hidden) {
            Button(drive.isOverview ? "回到车头跟随" : "看全程") { drive.toggleOverview() }
            Button(drive.trafficOn ? "关掉路况颜色" : "显示路况颜色") { drive.toggleTraffic() }
            Button("返回首页修改设置") { dismiss() }
            Button("退出导航", role: .destructive) { exitNavi() }
            Button("取消", role: .cancel) {}
        }
    }

    private func exitNavi() {
        navi.stop()
        dismiss()
    }

    /// 玻璃圆按钮直径：≈ 高德导航视图里车速圈的直径。
    /// 车速圈是 AMapNaviDriveView 自带控件，SDK 不暴露它的 frame / 尺寸，无法精确读取，
    /// 这里按默认布局（车速圈在左下）近似；改这一个常量即可整体微调两个按钮的大小。
    private let glassButtonSize: CGFloat = 64
    /// 按钮组距左边缘（对齐车速圈中心用）；真机上按车速圈实际位置微调。
    private let glassButtonLeading: CGFloat = 20
    /// 按钮组距底部的距离：让按钮落在车速圈正下方，同时避开高德自带底栏。
    private let glassButtonBottom: CGFloat = 96

    /// 导航时浮在高德界面上的控制：车速圈正下方竖排两个圆形玻璃按钮，从上到下先「设置」后「播报」。
    /// · 播报按钮 = 是否播报的开关（静音），点击不再弹出任何选项浮层；
    /// · 设置按钮沿用原行为：返回首页 / 设置。
    /// 模拟导航的「暂停 / 继续」「车速」是 iOS v0.6 的功能，保留在按钮组下方一行，不随本次改动删除。
    private var controlStack: some View {
        VStack(alignment: .leading, spacing: 12) {
            glassButton("gearshape.fill", active: false, label: "设置") { dismiss() }
            glassButton(settings.voiceMuted ? "speaker.slash.fill" : "speaker.wave.2.fill",
                        active: settings.voiceMuted,
                        label: settings.voiceMuted ? "已静音，点此恢复播报" : "播报，点此静音") {
                settings.voiceMuted.toggle()
                if settings.voiceMuted { Speech.shared.stopNow() }
            }
            if navi.emulating {
                HStack(spacing: 10) {
                    chip(navi.emuPaused ? "play.fill" : "pause.fill", navi.emuPaused ? "继续" : "暂停", on: navi.emuPaused) { navi.toggleEmuPause() }
                    chip("speedometer", "\(navi.emuSpeed) km/h", on: false) { navi.cycleEmuSpeed() }
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.leading, glassButtonLeading)
    }

    /// 圆形玻璃质感、只有图标没有文字的按钮（和 CardView 一样用 .ultraThinMaterial）
    private func glassButton(_ icon: String, active: Bool, label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: glassButtonSize * 0.42, weight: .semibold))
                .foregroundColor(active ? Color(red: 1, green: 0.31, blue: 0.71) : .white)
                .frame(width: glassButtonSize, height: glassButtonSize)
                .background(
                    ZStack {
                        Circle().fill(.ultraThinMaterial)
                        Circle().fill(Color.white.opacity(0.08))
                    }
                )
                .overlay(Circle().stroke(Color.white.opacity(0.35), lineWidth: 1))
                .clipShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(label))       // 纯图标按钮：保留无障碍标签
        .shadow(color: Color.black.opacity(0.28), radius: 8, x: 0, y: 3)
    }

    private func chip(_ icon: String, _ title: String, on: Bool, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: icon)
                .font(.subheadline.bold())
                .foregroundColor(on ? Color(red: 1, green: 0.31, blue: 0.71) : .white)
                .padding(.horizontal, 14).padding(.vertical, 9)
                .background(Capsule().fill(Color.black.opacity(0.82)))
        }
        .buttonStyle(.plain)
    }

    /// 到达目的地：显示 4 秒后自动关掉（点一下马上关）
    private var arrivedCard: some View {
        VStack(spacing: 8) {
            Image(systemName: "flag.checkered").font(.system(size: 40, weight: .bold)).foregroundColor(Color(red: 1, green: 0.31, blue: 0.71))
            Text(navi.info).font(.title2.bold()).foregroundColor(.white)
            Text("马上回到主界面").font(.footnote).foregroundColor(.gray)
        }
        .padding(28)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 20).fill(Color.black.opacity(0.88)))
        .padding(24)
        .frame(maxHeight: .infinity)
        .contentShape(Rectangle())
        .onTapGesture { exitNavi() }
    }
}
