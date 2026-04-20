// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "brightness",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "brightness",
            path: ".",
            exclude: ["Package.swift"],
            sources: ["main.swift"]
        )
    ]
)
