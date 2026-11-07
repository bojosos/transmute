//
//  PartialCompletion.swift
//  
//
//  Created by Sergej Jaskiewicz on 22.09.2019.
//

internal enum PartialCompletion<Value, Failure: Error> {

    case `continue`(Value)

    case finished

    case failure(Failure)
}

extension PartialCompletion where Value == Void {

    internal static var `continue`: PartialCompletion { return .continue(()) }
}
