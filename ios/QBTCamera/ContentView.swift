import SwiftUI

enum ControlTab: String, CaseIterable, Identifiable {
    case exposure, color, background, stream

    var id: String { rawValue }

    var title: String {
        switch self {
        case .exposure: "Экспозиция"
        case .color: "Цвет"
        case .background: "Фон"
        case .stream: "Поток"
        }
    }

    var icon: String {
        switch self {
        case .exposure: "slider.horizontal.3"
        case .color: "sparkles"
        case .background: "person.and.background.dotted"
        case .stream: "bolt.fill"
        }
    }
}

/// Full-screen viewfinder with Liquid Glass controls on top (QBT Studio design).
struct ContentView: View {
    @EnvironmentObject private var engine: StreamEngine
    @State private var tab = ControlTab.exposure
    @State private var showGrid = false
    @State private var editingPrompter = false
    @State private var pinchStartZoom: Double?
    @State private var savedBrightness: CGFloat = 0.5
    @State private var appeared = false
    @Namespace private var lensSelection

    private static let spring = Animation.spring(response: 0.42, dampingFraction: 0.82)

    var body: some View {
        ZStack {
            Palette.background.ignoresSafeArea()
            ProcessedPreview(sink: engine.preview)
                .ignoresSafeArea()
                .gesture(pinchToZoom)
                .onTapGesture(count: 2) { withAnimation(Self.spring) { engine.panelExpanded.toggle() } }
            if showGrid {
                GridOverlay()
                    .ignoresSafeArea()
                    .allowsHitTesting(false)
                    .transition(.opacity)
            }

            VStack(spacing: 10) {
                StatusIsland()
                    .offset(y: appeared ? 0 : -60)
                topRow
                    .offset(y: appeared ? 0 : -40)
                if engine.prompterVisible {
                    PrompterCard(editing: $editingPrompter)
                        .transition(.move(edge: .top).combined(with: .opacity).combined(with: .scale(scale: 0.95, anchor: .top)))
                }
                if let error = engine.errorMessage {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(10)
                        .glass(in: RoundedRectangle(cornerRadius: 16, style: .continuous), tint: Palette.danger.opacity(0.4))
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
                if let notice = engine.notice {
                    Label(notice, systemImage: "info.circle.fill")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(10)
                        .glass(in: RoundedRectangle(cornerRadius: 16, style: .continuous), tint: Palette.accent.opacity(0.35))
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
                Spacer(minLength: 0)
                lensRow
                    .offset(y: appeared ? 0 : 80)
                ControlPanelCard(tab: $tab, expanded: $engine.panelExpanded)
                    .offset(y: appeared ? 0 : 300)
            }
            .opacity(appeared ? 1 : 0)
            .padding(.horizontal, 12)
            .padding(.vertical, 4)
            .animation(Self.spring, value: engine.prompterVisible)
            .animation(Self.spring, value: engine.errorMessage)
            .animation(Self.spring, value: engine.notice)

            if engine.blackout {
                BlackoutView { setBlackout(false) }
                    .transition(.opacity)
            }
        }
        .onAppear {
            withAnimation(.spring(response: 0.7, dampingFraction: 0.8).delay(0.15)) { appeared = true }
        }
        .onChange(of: engine.blackout) { _, on in
            // Also covers the PC switching the screen off/on.
            if on {
                let current = UIScreen.main.brightness
                if current > 0.02 { savedBrightness = current }
                UIScreen.main.brightness = 0
            } else {
                UIScreen.main.brightness = max(savedBrightness, 0.3)
            }
        }
        .preferredColorScheme(.dark)
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .sheet(isPresented: $editingPrompter) {
            PrompterEditor().environmentObject(engine)
        }
    }

    private var topRow: some View {
        HStack(spacing: 8) {
            Button {
                withAnimation(Self.spring) {
                    tab = .stream
                    engine.panelExpanded = true
                }
            } label: {
                HStack(spacing: 8) {
                    QBTLogo(size: 22)
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 5) {
                            Text("\(engine.preset.resolution.title) · \(engine.preset.fps)")
                                .font(.system(size: 12, weight: .bold))
                                .contentTransition(.numericText())
                            Text("H.264")
                                .font(.system(size: 8, weight: .heavy))
                                .padding(.horizontal, 5)
                                .padding(.vertical, 1)
                                .background(Palette.accent, in: Capsule())
                        }
                        Text(engine.effectSettings.isActive ? "Эффекты включены" : "Rec.709 · без водяных знаков")
                            .font(.system(size: 9))
                            .foregroundStyle(Palette.secondaryText)
                            .contentTransition(.opacity)
                    }
                }
                .foregroundStyle(.white)
                .padding(.leading, 6)
                .padding(.trailing, 12)
                .padding(.vertical, 6)
                .glass(in: Capsule(), interactive: true)
                .animation(.snappy, value: engine.preset)
                .animation(.snappy, value: engine.effectSettings.isActive)
            }
            .buttonStyle(PressableStyle())

            Spacer(minLength: 0)
            GlassIconButton(systemImage: "text.alignleft", active: engine.prompterVisible) {
                engine.prompterVisible.toggle()
            }
            GlassIconButton(systemImage: "grid", active: showGrid) {
                withAnimation(.easeInOut(duration: 0.25)) { showGrid.toggle() }
            }
            if engine.cameraState.hasTorch {
                let torchOn = engine.cameraState.torch > 0
                let torchBlocked = !engine.cameraState.torchAvailable && !torchOn
                GlassIconButton(systemImage: torchOn ? "flashlight.on.fill" : torchBlocked ? "flashlight.slash" : "flashlight.off.fill",
                                active: torchOn) {
                    if torchBlocked {
                        engine.showNotice("Фонарик недоступен: iPhone нагрелся. Дайте ему остыть.")
                    } else {
                        engine.perform(["cmd": "torch", "value": torchOn ? 0.0 : 0.5])
                    }
                }
                .opacity(torchBlocked ? 0.55 : 1)
                .transition(.scale.combined(with: .opacity))
            }
            GlassIconButton(systemImage: "moon.fill") { setBlackout(true) }
        }
        .animation(Self.spring, value: engine.cameraState.hasTorch)
    }

    private var lensRow: some View {
        HStack(spacing: 8) {
            HStack(spacing: 2) {
                ForEach(CameraLens.available) { lens in
                    let selected = engine.lens == lens
                    Button {
                        withAnimation(Self.spring) { engine.lens = lens }
                    } label: {
                        Text(lens.title)
                            .font(.system(size: 13, weight: .bold))
                            .foregroundStyle(selected ? .white : .white.opacity(0.65))
                            .padding(.horizontal, 12)
                            .padding(.vertical, 7)
                            .background {
                                if selected {
                                    // The highlight slides from lens to lens.
                                    Capsule()
                                        .fill(Palette.accent.opacity(0.85))
                                        .matchedGeometryEffect(id: "lens", in: lensSelection)
                                }
                            }
                    }
                    .buttonStyle(PressableStyle())
                }
            }
            .padding(3)
            .glass(in: Capsule())

            if engine.cameraState.zoom > 1.01 {
                Text(String(format: "%.1f×", engine.cameraState.zoom))
                    .font(.system(size: 11, weight: .bold, design: .rounded))
                    .contentTransition(.numericText(value: engine.cameraState.zoom))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 6)
                    .glass(in: Capsule())
                    .transition(.scale.combined(with: .opacity))
            }
            Spacer(minLength: 0)
            GlassIconButton(systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right",
                            active: engine.effectSettings.mirror) {
                var settings = engine.effectSettings
                settings.mirror.toggle()
                engine.updateEffects(settings)
            }
            GlassIconButton(systemImage: "rotate.right", active: engine.orientation != .auto) {
                engine.orientation = nextOrientation
            }
        }
        .animation(Self.spring, value: engine.cameraState.zoom > 1.01)
        .animation(.snappy, value: engine.cameraState.zoom)
    }

    /// Rotate button: auto → landscape → portrait → landscape upside down → auto.
    private var nextOrientation: VideoOrientation {
        let order: [VideoOrientation] = [.auto, .landscape, .portrait, .landscapeFlipped]
        let index = order.firstIndex(of: engine.orientation) ?? 0
        return order[(index + 1) % order.count]
    }

    private var pinchToZoom: some Gesture {
        MagnifyGesture()
            .onChanged { value in
                let start = pinchStartZoom ?? engine.cameraState.zoom
                if pinchStartZoom == nil { pinchStartZoom = start }
                let state = engine.cameraState
                let zoom = (start * value.magnification).clamped(state.minZoom, state.maxZoom)
                engine.perform(["cmd": "zoom", "value": zoom])
            }
            .onEnded { _ in pinchStartZoom = nil }
    }

    /// Black screen with the stream still running: the display is the biggest heat source on long calls.
    private func setBlackout(_ on: Bool) {
        withAnimation(.easeInOut(duration: 0.3)) { engine.blackout = on }
    }
}

/// Bottom glass card. The dock always stays; the controls above it fold away (handle, drag,
/// tapping the active tab again, or double-tapping the picture).
struct ControlPanelCard: View {
    @Binding var tab: ControlTab
    @Binding var expanded: Bool
    @Namespace private var dockSelection
    @GestureState private var drag: CGFloat = 0

    private static let spring = Animation.spring(response: 0.42, dampingFraction: 0.82)

    var body: some View {
        VStack(spacing: 10) {
            handle
            if expanded {
                VStack(spacing: 12) {
                    Group {
                        switch tab {
                        case .exposure: ExposurePanel()
                        case .color: ColorPanel()
                        case .background: BackgroundPanel()
                        case .stream: StreamPanel()
                        }
                    }
                    .id(tab)
                    .transition(.asymmetric(insertion: .opacity.combined(with: .offset(y: 14)), removal: .opacity))
                    .frame(maxWidth: .infinity, alignment: .leading)

                    Rectangle().fill(.white.opacity(0.1)).frame(height: 1)
                }
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            dock
        }
        .padding(.horizontal, 14)
        .padding(.top, 8)
        .padding(.bottom, 14)
        .glass(in: RoundedRectangle(cornerRadius: 30, style: .continuous))
        // Rubber-band follow while dragging; the spring takes over on release.
        .offset(y: drag > 0 ? drag * 0.35 : drag * 0.12)
        .simultaneousGesture(
            DragGesture(minimumDistance: 14)
                .updating($drag) { value, state, _ in
                    if abs(value.translation.height) > abs(value.translation.width) { state = value.translation.height }
                }
                .onEnded { value in
                    guard abs(value.translation.height) > abs(value.translation.width) else { return }
                    if value.translation.height > 50 { setExpanded(false) }
                    if value.translation.height < -50 { setExpanded(true) }
                }
        )
        .animation(.interactiveSpring(response: 0.3, dampingFraction: 0.85), value: drag)
    }

    private var handle: some View {
        Button { setExpanded(!expanded) } label: {
            Image(systemName: "chevron.compact.down")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.white.opacity(0.55))
                .rotationEffect(.degrees(expanded ? 0 : 180))
                .frame(maxWidth: .infinity)
                .frame(height: 16)
                .contentShape(Rectangle())
        }
        .buttonStyle(PressableStyle())
    }

    private var dock: some View {
        HStack(spacing: 4) {
            ForEach(ControlTab.allCases) { item in
                let selected = tab == item
                Button {
                    withAnimation(Self.spring) {
                        if selected {
                            expanded.toggle() // tapping the open tab again folds the panel
                        } else {
                            tab = item
                            expanded = true
                        }
                    }
                } label: {
                    VStack(spacing: 4) {
                        Image(systemName: item.icon)
                            .font(.system(size: 16, weight: .semibold))
                            .symbolEffect(.bounce, value: selected)
                        Text(item.title).font(.system(size: 10, weight: .bold))
                    }
                    .foregroundStyle(selected ? .white : .white.opacity(0.55))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background {
                        if selected {
                            // The highlight slides between tabs.
                            RoundedRectangle(cornerRadius: 16, style: .continuous)
                                .fill(Palette.accent.opacity(expanded ? 0.45 : 0.25))
                                .matchedGeometryEffect(id: "tab", in: dockSelection)
                        }
                    }
                }
                .buttonStyle(PressableStyle())
            }
        }
    }

    private func setExpanded(_ value: Bool) {
        guard value != expanded else { return }
        withAnimation(Self.spring) { expanded = value }
    }
}
