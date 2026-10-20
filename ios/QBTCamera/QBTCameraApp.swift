import SwiftUI

@main
struct QBTCameraApp: App {
    @StateObject private var engine = StreamEngine()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
                .onAppear {
                    UIApplication.shared.isIdleTimerDisabled = true // auto-lock would kill the camera
                    UIDevice.current.beginGeneratingDeviceOrientationNotifications()
                    engine.start()
                }
                .onChange(of: scenePhase) { _, phase in
                    switch phase {
                    case .active:
                        UIApplication.shared.isIdleTimerDisabled = true
                        engine.resume()
                    case .background:
                        engine.pause()
                    default:
                        break
                    }
                }
        }
    }
}
