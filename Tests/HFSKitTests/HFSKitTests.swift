import Foundation
import Testing
@testable import HFSKit

@Test func copyOutSampleFile() async throws {
    let imgURL = try testImageURL()
    let volume = try HFSVolume(path: imgURL, writable: false)
    let tempDir = try makeTempDir()

    let attributes = try volume.attributes(of: ":Sample")
    #expect(attributes.name == "Sample")
    #expect(attributes.path == ":Sample")
    #expect(attributes.isDirectory == false)
    #expect(attributes.fileType == "????")
    #expect(attributes.fileCreator == "UNIX")
    guard let baseCreatedUTC = Calendar(identifier: .gregorian).date(
        from: DateComponents(
            timeZone: TimeZone(secondsFromGMT: 0),
            year: 2026,
            month: 1,
            day: 31,
            hour: 0,
            minute: 25,
            second: 57
        )
    ) else {
        throw HFSError.invalidArgument("Failed to construct expected creation date")
    }
    let expectedCreated = baseCreatedUTC.addingTimeInterval(
        -TimeInterval(TimeZone.current.secondsFromGMT(for: baseCreatedUTC))
    )
    #expect(attributes.created == expectedCreated)
    #expect(attributes.modified == expectedCreated)

    let outputURL = tempDir.appendingPathComponent("Sample")
    let copyOutPaths = ["Sample", ":Sample"]
    var lastError: Error?
    for path in copyOutPaths {
        do {
            try volume.copyOut(hfsPath: path, toHostPath: outputURL)
            lastError = nil
            break
        } catch {
            lastError = error
        }
    }
    if let lastError {
        throw lastError
    }

    let data = try Data(contentsOf: outputURL)
    let text = String(data: data, encoding: .utf8)
    #expect(text?.trimmingCharacters(in: .whitespacesAndNewlines) == "Hello World!")
    #expect(attributes.dataForkSize == data.count)
}

@Test func copyInMountainFile() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()
    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")

    let attributes = try volume.attributes(of: "mountain")
    #expect(attributes.name == "mountain")
    #expect(attributes.isDirectory == false)
    #expect(attributes.fileType == "????")
    #expect(attributes.fileCreator == "UNIX")
    #expect(attributes.created.timeIntervalSince1970 > 0)
    #expect(attributes.modified.timeIntervalSince1970 >= attributes.created.timeIntervalSince1970)

    let hostData = try Data(contentsOf: mountainURL)
    #expect(attributes.dataForkSize == hostData.count)
    #expect(attributes.resourceForkSize == 0)
}

@Test func listAndDeleteMountainFile() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()
    let infoBefore = try volume.volumeInfo()
    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")

    let entriesAfterAdd = try volume.list(directory: ":")
    let mountainEntry = entriesAfterAdd.first { $0.name == "mountain" }
    #expect(mountainEntry != nil)
    #expect(mountainEntry?.isDirectory == false)
    #expect(mountainEntry?.path == ":mountain")
    let hostData = try Data(contentsOf: mountainURL)
    #expect(mountainEntry?.dataForkSize == hostData.count)
    #expect(mountainEntry?.fileType == "????")
    #expect(mountainEntry?.fileCreator == "UNIX")
    let infoAfterAdd = try volume.volumeInfo()
    #expect(infoAfterAdd.usedBytes > infoBefore.usedBytes)

    if let info = try volume.list(directory: ":").first(where: { $0.name == "mountain" }) {
        try volume.delete(info)
    } else {
        throw HFSError.invalidArgument("Expected mountain in root listing")
    }

    let entriesAfterDelete = try volume.list(directory: ":")
    #expect(!entriesAfterDelete.contains { $0.name == "mountain" })
    let infoAfterDelete = try volume.volumeInfo()
    #expect(infoAfterDelete.usedBytes <= infoAfterAdd.usedBytes)
    #expect(infoAfterDelete.usedBytes >= infoBefore.usedBytes)
}

@Test func copyInOutMountainRoundTrip() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()
    let tempDir = try makeTempDir()

    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")

    let outputURL = tempDir.appendingPathComponent("mountain.out")
    let fileInfo = try volume.attributes(of: "mountain")
    try volume.copyOut(hfsPath: fileInfo, toHostPath: outputURL)

    let inputData = try Data(contentsOf: mountainURL)
    let outputData = try Data(contentsOf: outputURL)
    #expect(outputData == inputData)
}

@Test func renameMountainFile() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")
    try volume.rename(path: "mountain", to: "mountain2")

    let entries = try volume.list(directory: ":")
    #expect(entries.contains { $0.name == "mountain2" })
    #expect(!entries.contains { $0.name == "mountain" })
}

@Test func moveFileToDirectory() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")
    try volume.makeDirectory(path: ":Folder")
    try volume.move(path: "mountain", toParentDirectory: ":Folder")

    let rootEntries = try volume.list(directory: ":")
    #expect(!rootEntries.contains { $0.name == "mountain" })

    let folderEntries = try volume.list(directory: ":Folder")
    #expect(folderEntries.contains { $0.name == "mountain" && !$0.isDirectory })
}

@Test func moveDirectoryToDirectory() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.makeDirectory(path: ":FolderA")
    try volume.makeDirectory(path: ":FolderB")
    try volume.copyIn(hostPath: mountainURL, toHFSPath: ":FolderA:mountain")

    let folderInfo = try volume.attributes(of: ":FolderA")
    try volume.move(folderInfo, toParentDirectory: ":FolderB")

    let rootEntries = try volume.list(directory: ":")
    #expect(!rootEntries.contains { $0.name == "FolderA" })

    let folderBEntries = try volume.list(directory: ":FolderB")
    #expect(folderBEntries.contains { $0.name == "FolderA" && $0.isDirectory })

    let movedEntries = try volume.list(directory: ":FolderB:FolderA")
    #expect(movedEntries.contains { $0.name == "mountain" && !$0.isDirectory })
}

@Test func nestedPathOperations() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.makeDirectory(path: ":Folder")
    try volume.copyIn(hostPath: mountainURL, toHFSPath: ":Folder:mountain")

    let rootEntries = try volume.list(directory: ":")
    #expect(rootEntries.contains { $0.name == "Folder" && $0.isDirectory })

    let folderEntries = try volume.list(directory: ":Folder")
    #expect(folderEntries.contains { $0.name == "mountain" && !$0.isDirectory })

    let fileInfo = try volume.attributes(of: ":Folder:mountain")
    #expect(fileInfo.name == "mountain")
}

@Test func relativeAndAbsolutePathsMatch() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")

    let absInfo = try volume.attributes(of: ":mountain")
    let relInfo = try volume.attributes(of: "mountain")
    #expect(absInfo.dataForkSize == relInfo.dataForkSize)
    #expect(absInfo.fileType == relInfo.fileType)
    #expect(absInfo.fileCreator == relInfo.fileCreator)
}

@Test func setTypeCreatorUpdatesAttributes() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.copyIn(hostPath: mountainURL, toHFSPath: "mountain")
    try volume.setTypeCreator(path: "mountain", fileType: "TEXT", fileCreator: "ttxt")

    let info = try volume.attributes(of: "mountain")
    #expect(info.fileType == "TEXT")
    #expect(info.fileCreator == "ttxt")
}

@Test func errorCases() async throws {
    let imgURL = try testImageURL()
    let mountainURL = try mountainURL()

    let readOnly = try HFSVolume(path: imgURL, writable: false)
    expectThrows {
        try readOnly.copyIn(hostPath: mountainURL, toHFSPath: "mountain")
    }
    expectThrows {
        try readOnly.delete(path: ":Sample")
    }

    expectThrows {
        _ = try readOnly.attributes(of: ":DoesNotExist")
    }
    let tempDir = try makeTempDir()
    let outputURL = tempDir.appendingPathComponent("missing.out")
    expectThrows {
        try readOnly.copyOut(hfsPath: ":DoesNotExist", toHostPath: outputURL)
    }
}

@Test func deleteDirectoryWithContentsFails() async throws {
    let mountainURL = try mountainURL()
    let volume = try makeWritableVolume()

    try volume.makeDirectory(path: ":Folder")
    try volume.copyIn(hostPath: mountainURL, toHFSPath: ":Folder:mountain")

    expectThrows {
        try volume.delete(path: ":Folder")
    }
}

@Test func copyInOutDirectoryAndDeleteRecursively() async throws {
    let volume = try makeWritableVolume()
    let tempDir = try makeTempDir()
    let sourceDir = tempDir.appendingPathComponent("SourceDir", isDirectory: true)
    let outputDir = tempDir.appendingPathComponent("OutputDir", isDirectory: true)

    let (rootFileURL, nestedFileURL) = try createHostDirectoryFixture(at: sourceDir)

    try volume.copyInDirectory(hostDirectory: sourceDir, toHFSPath: ":DirFixture")

    let rootEntries = try volume.list(directory: ":")
    #expect(rootEntries.contains { $0.name == "DirFixture" && $0.isDirectory })
    let fixtureEntries = try volume.list(directory: ":DirFixture")
    #expect(fixtureEntries.contains { $0.name == rootFileURL.lastPathComponent && !$0.isDirectory })
    #expect(fixtureEntries.contains { $0.name == "Sub" && $0.isDirectory })

    let nestedEntries = try volume.list(directory: ":DirFixture:Sub")
    #expect(nestedEntries.contains { $0.name == nestedFileURL.lastPathComponent && !$0.isDirectory })

    let fixtureInfo = try volume.attributes(of: ":DirFixture")
    try volume.copyOut(hfsPath: fixtureInfo, toHostPath: outputDir)

    let copiedRoot = outputDir.appendingPathComponent(rootFileURL.lastPathComponent)
    let copiedNested = outputDir
        .appendingPathComponent("Sub", isDirectory: true)
        .appendingPathComponent(nestedFileURL.lastPathComponent)
    #expect(try Data(contentsOf: copiedRoot) == Data(contentsOf: rootFileURL))
    #expect(try Data(contentsOf: copiedNested) == Data(contentsOf: nestedFileURL))

    try volume.deleteDirectory(fixtureInfo)
    let entriesAfterDelete = try volume.list(directory: ":")
    #expect(!entriesAfterDelete.contains { $0.name == "DirFixture" })
}

@Test func copyInOutDirectoryWithEmptyAndSpacedNames() async throws {
    let volume = try makeWritableVolume()
    let tempDir = try makeTempDir()
    let sourceDir = tempDir.appendingPathComponent("SourceComplex", isDirectory: true)
    let outputDir = tempDir.appendingPathComponent("OutputComplex", isDirectory: true)

    let (rootFileURL, nestedFileURL, emptyDirURL) = try createComplexHostDirectoryFixture(at: sourceDir)
    try volume.copyInDirectory(hostDirectory: sourceDir, toHFSPath: ":DirComplex")

    let complexEntries = try volume.list(directory: ":DirComplex")
    #expect(complexEntries.contains { $0.name == rootFileURL.lastPathComponent })
    #expect(complexEntries.contains { $0.name == emptyDirURL.lastPathComponent && $0.isDirectory })

    try volume.copyOutDirectory(hfsPath: ":DirComplex", toHostDirectory: outputDir)

    let copiedRoot = outputDir.appendingPathComponent(rootFileURL.lastPathComponent)
    let copiedNested = outputDir
        .appendingPathComponent("Sub", isDirectory: true)
        .appendingPathComponent(nestedFileURL.lastPathComponent)
    let copiedEmptyDir = outputDir.appendingPathComponent(emptyDirURL.lastPathComponent, isDirectory: true)

    #expect(try Data(contentsOf: copiedRoot) == Data(contentsOf: rootFileURL))
    #expect(try Data(contentsOf: copiedNested) == Data(contentsOf: nestedFileURL))
    var isDir: ObjCBool = false
    #expect(FileManager.default.fileExists(atPath: copiedEmptyDir.path, isDirectory: &isDir) && isDir.boolValue)
}

@Test func overwriteFileReplacesContents() async throws {
    let volume = try makeWritableVolume()
    let tempDir = try makeTempDir()
    let firstURL = tempDir.appendingPathComponent("first.txt")
    let secondURL = tempDir.appendingPathComponent("second.txt")

    try Data("first".utf8).write(to: firstURL)
    try Data("second-contents".utf8).write(to: secondURL)

    try volume.copyIn(hostPath: firstURL, toHFSPath: "overwrite")
    try volume.copyIn(hostPath: secondURL, toHFSPath: "overwrite")

    let info = try volume.attributes(of: "overwrite")
    #expect(info.dataForkSize == Data("second-contents".utf8).count)
}

@Test func largeFileCopyInOut() async throws {
    let volume = try makeWritableVolume()
    let tempDir = try makeTempDir()
    let largeURL = tempDir.appendingPathComponent("large.bin")
    let outputURL = tempDir.appendingPathComponent("large.out")

    let data = Data(repeating: 0xA5, count: 20000)
    try data.write(to: largeURL)

    try volume.copyIn(hostPath: largeURL, toHFSPath: "large.bin")
    try volume.copyOut(hfsPath: "large.bin", toHostPath: outputURL)

    let outData = try Data(contentsOf: outputURL)
    #expect(outData == data)
}

@Test func readOnlyDirectoryOperationsFail() async throws {
    let imgURL = try testImageURL()
    let volume = try HFSVolume(path: imgURL, writable: false)
    let tempDir = try makeTempDir()
    let sourceDir = tempDir.appendingPathComponent("RODir", isDirectory: true)
    let (_, _, _) = try createComplexHostDirectoryFixture(at: sourceDir)

    expectThrows {
        try volume.copyInDirectory(hostDirectory: sourceDir, toHFSPath: ":ReadOnlyDir")
    }
    expectThrows {
        try volume.deleteDirectory(path: ":System Folder")
    }
}

@Test func listTest2ImageContents() async throws {
    let imgURL = try test2ImageURL()
    let volume = try HFSVolume(path: imgURL, writable: false)

    let rootEntries = try volume.list(directory: ":")
    #expect(rootEntries.count == 3)

    let checksum = rootEntries.first { $0.name == "Checksum results" }
    #expect(checksum != nil)
    #expect(checksum?.isDirectory == false)
    #expect(checksum?.fileType == "TEXT")
    #expect(checksum?.fileCreator == "ttxt")

    let journey1987 = rootEntries.first { $0.name == "The Journey (1987)" }
    #expect(journey1987 != nil)
    #expect(journey1987?.isDirectory == true)

    let journey1988 = rootEntries.first { $0.name == "The Journey (1988)" }
    #expect(journey1988 != nil)
    #expect(journey1988?.isDirectory == true)

    for entry in rootEntries {
        #expect(!entry.path.isEmpty)
        _ = try volume.attributes(of: entry.path)
        if entry.isDirectory {
            _ = try volume.list(directory: entry.path)
        }
    }
}

@Test func copyInMacBinaryAsRaw() async throws {
    let volume = try makeWritableVolume()
    let binURL = try sunglassesURL()

    try volume.copyIn(hostPath: binURL, toHFSPath: "sunglasses.bin")
    let info = try volume.attributes(of: "sunglasses.bin")
    let binData = try Data(contentsOf: binURL)
    #expect(info.dataForkSize == binData.count)
    #expect(info.fileType == "????")
    #expect(info.fileCreator == "UNIX")
}

private func testImageURL() throws -> URL {
    guard let imgURL = Bundle.module.url(forResource: "test", withExtension: "img") else {
        throw HFSError.invalidArgument("Missing test image resource")
    }
    return imgURL
}

private func mountainURL() throws -> URL {
    guard let url = Bundle.module.url(forResource: "mountain", withExtension: nil) else {
        throw HFSError.invalidArgument("Missing mountain resource")
    }
    return url
}

private func sunglassesURL() throws -> URL {
    guard let url = Bundle.module.url(forResource: "sunglasses", withExtension: "bin") else {
        throw HFSError.invalidArgument("Missing sunglasses resource")
    }
    return url
}

private func test2ImageURL() throws -> URL {
    guard let imgURL = Bundle.module.url(forResource: "test2", withExtension: "img") else {
        throw HFSError.invalidArgument("Missing test2 image resource")
    }
    return imgURL
}

private func makeTempDir() throws -> URL {
    let tempDir = FileManager.default.temporaryDirectory
        .appendingPathComponent("HFSKitTests-\(UUID())", isDirectory: true)
    try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
    return tempDir
}

private func makeWritableVolume() throws -> HFSVolume {
    let imgURL = try testImageURL()
    let tempDir = try makeTempDir()
    let writableImageURL = tempDir.appendingPathComponent("test.img")
    try FileManager.default.copyItem(at: imgURL, to: writableImageURL)
    return try HFSVolume(path: writableImageURL, writable: true)
}

private func expectThrows(_ block: () throws -> Void) {
    do {
        try block()
        #expect(Bool(false))
    } catch {
        #expect(Bool(true))
    }
}

private func createHostDirectoryFixture(at url: URL) throws -> (URL, URL) {
    let fm = FileManager.default
    try fm.createDirectory(at: url, withIntermediateDirectories: true)

    let rootFile = url.appendingPathComponent("root.txt")
    try Data("root file".utf8).write(to: rootFile)

    let subDir = url.appendingPathComponent("Sub", isDirectory: true)
    try fm.createDirectory(at: subDir, withIntermediateDirectories: true)

    let nestedFile = subDir.appendingPathComponent("nested.txt")
    try Data("nested file".utf8).write(to: nestedFile)

    return (rootFile, nestedFile)
}

private func createComplexHostDirectoryFixture(at url: URL) throws -> (URL, URL, URL) {
    let fm = FileManager.default
    try fm.createDirectory(at: url, withIntermediateDirectories: true)

    let rootFile = url.appendingPathComponent("root file.txt")
    try Data("root file".utf8).write(to: rootFile)

    let subDir = url.appendingPathComponent("Sub", isDirectory: true)
    try fm.createDirectory(at: subDir, withIntermediateDirectories: true)

    let nestedFile = subDir.appendingPathComponent("nested.txt")
    try Data("nested file".utf8).write(to: nestedFile)

    let emptyDir = url.appendingPathComponent("EmptyDir", isDirectory: true)
    try fm.createDirectory(at: emptyDir, withIntermediateDirectories: true)

    return (rootFile, nestedFile, emptyDir)
}
