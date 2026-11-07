//
//  Subject+Void.swift
//  OpenCombine
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

extension Subject where Output == Void {

    public func send() {
        send(())
    }
}
