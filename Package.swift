// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "HFSKit",
    products: [
        // This is what your app will depend on
        .library(name: "HFSKit", targets: ["HFSKit"])
    ],
    targets: [
        // C target: libhfs + copyin/copyout + your wrapper
        .target(
            name: "HFSCore",
            path: "Sources/HFSCore",
            exclude: [
                "hfsutils/hcopy.c",
                "libhfs/os.c"
            ],
            publicHeadersPath: "include",
            cSettings: [
                .define("HAVE_CONFIG_H"),
                .headerSearchPath("libhfs"),
                .headerSearchPath("hfsutils")
                // add more if needed
            ]
        ),
        // Swift target: your friendly API
        .target(
            name: "HFSKit",
            dependencies: ["HFSCore"],
            path: "Sources/HFSKit"
        ),
        .testTarget(
            name: "HFSKitTests",
            dependencies: ["HFSKit"],
            resources: [
                    .copy("Resources/test.img"),
                    .copy("Resources/test2.img"),
                    .copy("Resources/mountain"),
                    .copy("Resources/sunglasses.bin")
                ]
        )
    ]
)
