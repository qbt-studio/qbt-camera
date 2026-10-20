import SwiftUI

@main
struct QBTCameraApp: App {
    @StateObject private var engine = StreamEngine()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
                .onAppear {
                    UIApplication.shared.isIdleTimerDisabled = true // auto-lock would kill the camera
                    UIDevice.current.beginGeneratingDeviceOrientationNotifications()
                    engine.start()
                }
        }
    }
}
