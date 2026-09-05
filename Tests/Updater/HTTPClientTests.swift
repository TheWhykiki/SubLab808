import AppKit
import Foundation

// A transport fixture. HTTPClient's actual URLSession delegate and completion
// code run unchanged; this protocol never opens a socket.
final class FixtureProtocol: URLProtocol {
    static var status = 200
    static var body = Data()
    static var headers: [String: String] = [:]
    static var error: Error?
    static var hold = false
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        if Self.hold { return }
        if let error = Self.error { client?.urlProtocol(self, didFailWithError: error); return }
        let response = HTTPURLResponse(url: request.url!, statusCode: Self.status, httpVersion: "HTTP/1.1", headerFields: Self.headers)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Self.body)
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
    static func configuration() -> URLSessionConfiguration {
        let result = URLSessionConfiguration.ephemeral
        result.protocolClasses = [FixtureProtocol.self]
        return result
    }
    static func reset(status: Int = 200, body: String = "{}") {
        Self.status = status; Self.body = Data(body.utf8); Self.headers = [:]; Self.error = nil; Self.hold = false
    }
}

@main struct HTTPClientTests {
    static var count = 0
    static func test(_ name: String, _ body: () throws -> Void) throws {
        FixtureProtocol.reset(); try body(); count += 1; print("PASS " + name)
    }
    static func drain(_ ready: () -> Bool) throws {
        let deadline = Date().addingTimeInterval(8)
        while !ready() && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        try require(ready(), "HTTP fixture did not complete")
    }
    static func request(limit: Int = 1024) throws -> Result<Data, Error> {
        var result: Result<Data, Error>?
        var completions = 0
        let client = HTTPClient(url: Product.reverse.apiURL, limit: limit, configuration: FixtureProtocol.configuration()) {
            completions += 1; result = $0
        }
        try drain { result != nil }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        try require(completions == 1, "Completion called more than once")
        withExtendedLifetime(client) {}
        return result!
    }
    static func failure(_ result: Result<Data, Error>) throws -> Error {
        switch result {
        case .failure(let error): return error
        case .success: throw UpdateFailure("Expected HTTP failure")
        }
    }
    static func main() throws {
        let application = NSApplication.shared
        application.setActivationPolicy(.prohibited)
        try test("real metadata delegate returns data exactly once") {
            FixtureProtocol.body = Data("release bytes".utf8)
            try require(try request().get() == FixtureProtocol.body, "Response body changed")
        }
        try test("metadata 404 is a distinct no-release state") {
            FixtureProtocol.status = 404
            let error = try failure(request())
            try require(error is HTTPFailure, "Missing release became generic download failure")
        }
        try test("rate limits and server errors remain failures") {
            for status in [403, 429, 500] {
                FixtureProtocol.status = status
                _ = try failure(request())
            }
        }
        try test("declared and streamed metadata size limits") {
            FixtureProtocol.headers = ["Content-Length": "2048"]
            _ = try failure(request(limit: 32))
            FixtureProtocol.headers = [:]
            FixtureProtocol.body = Data(repeating: 65, count: 2048)
            _ = try failure(request(limit: 32))
        }
        try test("offline transport error reaches the caller") {
            FixtureProtocol.error = URLError(.notConnectedToInternet)
            let error = try failure(request()) as NSError
            try require(error.code == URLError.notConnectedToInternet.rawValue, "Transport error replaced")
        }
        try test("cancel completes a pending request once") {
            FixtureProtocol.hold = true
            var result: Result<Data, Error>?
            var completions = 0
            let client = HTTPClient(url: Product.reverse.apiURL, limit: 1024, configuration: FixtureProtocol.configuration()) {
                result = $0; completions += 1
            }
            client.cancel()
            try drain { result != nil }
            _ = try failure(result!)
            try require(completions == 1, "Cancellation completed more than once")
        }
        try test("redirects reject foreign hosts, credentials, HTTP and excessive hops") {
            let valid = URL(string: "https://api.github.com/repos/TheWhykiki/ReverseLab/releases/latest")!
            for target in ["https://evil.example/release", "http://api.github.com/release", "https://user@api.github.com/release", "https://api.github.com:444/release", valid.absoluteString] {
                FixtureProtocol.hold = true
                var result: Result<Data, Error>?
                let client = HTTPClient(url: valid, limit: 1024, configuration: FixtureProtocol.configuration()) { result = $0 }
                let session = URLSession(configuration: FixtureProtocol.configuration())
                defer { session.invalidateAndCancel() }
                let task = session.dataTask(with: valid) // Intentionally never resumed.
                let response = HTTPURLResponse(url: valid, statusCode: 302, httpVersion: nil, headerFields: nil)!
                let hops = target == valid.absoluteString ? 6 : 1
                for index in 1...hops {
                    var accepted = false
                    client.urlSession(session, task: task, willPerformHTTPRedirection: response, newRequest: URLRequest(url: URL(string: target)!)) {
                        accepted = $0 != nil
                    }
                    try require(accepted == (target == valid.absoluteString && index <= 5), "Redirect policy mismatch")
                }
                try drain { result != nil }
                _ = try failure(result!)
            }
        }
        try test("download host policy is distinct from metadata policy") {
            for host in ["github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"] {
                let url = URL(string: "https://\(host)/asset")!
                try require(HTTPClient.allowed(url, metadata: false) && !HTTPClient.allowed(url, metadata: true), "Incorrect allowed hosts")
            }
        }
        try test("real controller distinguishes missing release and newer development build") {
            let app = UpdaterApp()
            app.product = .reverse; app.current = try Version("1.1.0")
            app.operations.httpConfiguration = FixtureProtocol.configuration
            FixtureProtocol.status = 404
            app.checkRelease()
            try drain { !app.busy }
            try require(app.status.stringValue == "Noch kein GitHub-Release" && app.step == .check, "No-release UI incorrect")
            FixtureProtocol.reset(body: "{\"tag_name\":\"v1.0.4\",\"draft\":false,\"prerelease\":false,\"assets\":[]}")
            app.checkRelease()
            try drain { !app.busy }
            try require(app.status.stringValue == "Installierte Version ist neuer" && app.detail.stringValue.contains("1.0.4"), "Development build mislabeled as latest release")
        }
        try test("malformed release data cannot enable installation") {
            FixtureProtocol.body = Data("not JSON".utf8)
            let app = UpdaterApp()
            app.product = .sublab; app.current = try Version("1.4.0")
            app.operations.httpConfiguration = FixtureProtocol.configuration
            app.checkRelease()
            try drain { !app.busy }
            try require(app.candidate == nil && app.step == .check, "Malformed release enabled installation")
        }
        print("\(count) HTTP and release-UI tests passed using URLProtocol fixtures; no network or Installer was used.")
    }
}
