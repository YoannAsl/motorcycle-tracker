# Motorcycle tracking

This context covers the data recorded by the motorcycle tracker and its delivery to remote storage. It stops at the boundary of any later ride-history or map application.

## Language

**Tracking session**:
The GPS observations recorded after movement begins and before the tracker loses power. A tracking session may cover only part of a ride.
_Avoid_: Ride, trip

**Track point**:
One fresh GPS observation recorded during a tracking session, including its time, position, movement, and fix quality. Weak fixes and stopped points remain track points.
_Avoid_: Sample, coordinate

**Pending data**:
Recorded data whose remote receipt has not yet been confirmed.
_Avoid_: New data, unsent data

**Delivered data**:
Recorded data whose remote receipt has been confirmed. Delivery does not remove the local copy.
_Avoid_: Uploaded data, synced data
