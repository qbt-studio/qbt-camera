import SwiftUI

/// QBT Studio palette (from the brand mockup).
enum Palette {
    static let accent = Color(red: 0.227, green: 0.525, blue: 1.0)       // #3A86FF
    static let logoStroke = Color(red: 0.169, green: 0.498, blue: 1.0)   // #2B7FFF
    static let logoBadge = Color(red: 0.325, green: 0.608, blue: 1.0)    // #539BFF
    static let background = Color(red: 0.020, green: 0.024, blue: 0.043) // #05060B
    static let live = Color(red: 0.204, green: 0.827, blue: 0.600)       // emerald
    static let warning = Color(red: 1.0, green: 0.690, blue: 0.125)
    static let danger = Color(red: 1.0, green: 0.333, blue: 0.333)
    static let secondaryText = Color.white.opacity(0.6)
}

/// QBT logo: rounded-square outline with a badge in the bottom-right corner (100x100 design grid).
struct QBTLogo: View {
    var size: CGFloat = 28

    var body: some View {
        Canvas { context, canvas in
            let unit = canvas.width / 100
            let outline = Path(roundedRect: CGRect(x: 11 * unit, y: 11 * unit, width: 72 * unit, height: 72 * unit),
                               cornerRadius: 24 * unit, style: .continuous)
            context.stroke(outline, with: .color(Palette.logoStroke), lineWidth: 10.5 * unit)
            let badge = Path(roundedRect: CGRect(x: 66 * unit, y: 66 * unit, width: 28 * unit, height: 28 * unit),
                             cornerRadius: 9 * unit, style: .continuous)
            context.fill(badge, with: .color(Palette.logoBadge))
        }
        .frame(width: size, height: size)
        .shadow(color: Palette.logoStroke.opacity(0.55), radius: size * 0.14)
    }
}

extension View {
    /// Liquid Glass on iOS 26+, frosted material on older systems.
    @ViewBuilder
    func glass<S: Shape>(in shape: S, tint: Color? = nil, interactive: Bool = false) -> some View {
        if #available(iOS 26.0, *) {
            glassEffect(liquidGlass(tint: tint, interactive: interactive), in: shape)
        } else {
            background((tint ?? .clear).opacity(0.5), in: shape)
                .background(.ultraThinMaterial, in: shape)
                .overlay(shape.stroke(.white.opacity(0.18), lineWidth: 1))
        }
    }
}

@available(iOS 26.0, *)
private func liquidGlass(tint: Color?, interactive: Bool) -> Glass {
    var glass = Glass.regular
    if let tint { glass = glass.tint(tint) }
    if interactive { glass = glass.interactive() }
    return glass
}

/// Springy press feedback for every button in the app.
struct PressableStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.92 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
    }
}

/// Capsule button: plain glass, or accent-tinted glass when active.
struct GlassChip: View {
    let title: String
    var systemImage: String? = nil
    var active = false
    var font: Font = .system(size: 13, weight: .semibold)
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if let systemImage { Image(systemName: systemImage) }
                Text(title).lineLimit(1)
            }
            .font(font)
            .foregroundStyle(active ? .white : .white.opacity(0.72))
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .glass(in: Capsule(), tint: active ? Palette.accent.opacity(0.55) : nil, interactive: true)
            .animation(.snappy(duration: 0.25), value: active)
        }
        .buttonStyle(PressableStyle())
    }
}

/// Round icon button in glass.
struct GlassIconButton: View {
    let systemImage: String
    var active = false
    var size: CGFloat = 36
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: size * 0.4, weight: .semibold))
                .foregroundStyle(active ? .white : .white.opacity(0.8))
                .symbolEffect(.bounce, value: active)
                .contentTransition(.symbolEffect(.replace))
                .frame(width: size, height: size)
                .glass(in: Circle(), tint: active ? Palette.accent.opacity(0.55) : nil, interactive: true)
                .animation(.snappy(duration: 0.25), value: active)
        }
        .buttonStyle(PressableStyle())
    }
}

/// Small uppercase section caption.
struct SectionCaption: View {
    let title: String
    var systemImage: String? = nil
    var tint: Color = Palette.accent

    var body: some View {
        HStack(spacing: 6) {
            if let systemImage { Image(systemName: systemImage).foregroundStyle(tint) }
            Text(title.uppercased()).tracking(0.8)
        }
        .font(.system(size: 11, weight: .bold))
        .foregroundStyle(.white.opacity(0.7))
    }
}
