#!/usr/bin/env python3
import random
import string
import pytest
from helpers.cluster import ClickHouseCluster
from helpers import keeper_utils
from kazoo.client import KazooClient, KazooState
from kazoo.exceptions import ConnectionLoss

cluster = ClickHouseCluster(__file__)

# clickhouse itself will use external zookeeper
node = cluster.add_instance(
    "node",
    main_configs=["configs/enable_keeper.xml"],
    stay_alive=True,
    with_zookeeper=True,
)


def random_string(length):
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=length))


def get_connection_zk(nodename, timeout=30.0):
    _fake_zk_instance = KazooClient(
        hosts=cluster.get_instance_ip(nodename) + ":9181", timeout=timeout
    )
    _fake_zk_instance.start()
    return _fake_zk_instance


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()

        yield cluster

    finally:
        cluster.shutdown()


def test_soft_limit_create(started_cluster):
    keeper_utils.wait_until_connected(started_cluster, node)
    try:
        node_zk = get_connection_zk("node")
        loop_time = 1000000

        for i in range(loop_time):
            node_zk.create("/test_soft_limit/node_" + str(i), random_string(100))
    except ConnectionLoss:
        txn = node_zk.transaction()
        for i in range(10):
            txn.delete("/test_soft_limit/node_" + str(i))

        txn.create("/test_soft_limit/node_1000001" + str(i), "abcde")
        txn.commit()
        return

    raise Exception("all records are inserted but no error occurs")
